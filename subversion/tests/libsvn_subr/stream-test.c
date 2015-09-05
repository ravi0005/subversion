/*
 * stream-test.c -- test the stream functions
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdio.h>
#include "svn_pools.h"
#include "svn_io.h"
#include "svn_subst.h"
#include "svn_base64.h"
#include <apr_general.h>

#include "private/svn_io_private.h"

#include "../svn_test.h"

struct stream_baton_t
{
  svn_filesize_t capacity_left;
  char current;
  apr_size_t max_read;
};

/* Implements svn_stream_t.read_fn. */
static svn_error_t *
read_handler(void *baton,
             char *buffer,
             apr_size_t *len)
{
  struct stream_baton_t *btn = baton;
  int i;

  /* Cap the read request to what we actually support. */
  if (btn->max_read < *len)
    *len = btn->max_read;
  if (btn->capacity_left < *len)
    *len = (apr_size_t)btn->capacity_left;

  /* Produce output */
  for (i = 0; i < *len; ++i)
    {
      buffer[i] = btn->current + 1;
      btn->current = (btn->current + 1) & 0x3f;

      btn->capacity_left--;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
data_available_handler(void *baton,
                       svn_boolean_t *data_available)
{
  struct stream_baton_t *btn = baton;
  *data_available = btn->capacity_left > 0;

  return SVN_NO_ERROR;
}

/* Return a stream that produces CAPACITY characters in chunks of at most
 * MAX_READ chars.  The first char will be '\1' followed by '\2' etc. up
 * to '\x40' and then repeating the cycle until the end of the stream.
 * Allocate the result in RESULT_POOL. */
static svn_stream_t *
create_test_read_stream(svn_filesize_t capacity,
                        apr_size_t max_read,
                        apr_pool_t *result_pool)
{
  svn_stream_t *stream;
  struct stream_baton_t *baton;

  baton = apr_pcalloc(result_pool, sizeof(*baton));
  baton->capacity_left = capacity;
  baton->current = 0;
  baton->max_read = max_read;

  stream = svn_stream_create(baton, result_pool);
  svn_stream_set_read2(stream, read_handler, NULL);
  svn_stream_set_data_available(stream, data_available_handler);

  return stream;
}

/*------------------------ Tests --------------------------- */

static svn_error_t *
test_stream_from_string(apr_pool_t *pool)
{
  int i;
  apr_pool_t *subpool = svn_pool_create(pool);

#define NUM_TEST_STRINGS 4
#define TEST_BUF_SIZE 10

  static const char * const strings[NUM_TEST_STRINGS] = {
    /* 0 */
    "",
    /* 1 */
    "This is a string.",
    /* 2 */
    "This is, by comparison to the previous string, a much longer string.",
    /* 3 */
    "And if you thought that last string was long, you just wait until "
    "I'm finished here.  I mean, how can a string really claim to be long "
    "when it fits on a single line of 80-columns?  Give me a break. "
    "Now, I'm not saying that I'm the longest string out there--far from "
    "it--but I feel that it is safe to assume that I'm far longer than my "
    "peers.  And that demands some amount of respect, wouldn't you say?"
  };

  /* Test svn_stream_from_stringbuf() as a readable stream. */
  for (i = 0; i < NUM_TEST_STRINGS; i++)
    {
      svn_stream_t *stream;
      char buffer[TEST_BUF_SIZE];
      svn_stringbuf_t *inbuf, *outbuf;
      apr_size_t len;

      inbuf = svn_stringbuf_create(strings[i], subpool);
      outbuf = svn_stringbuf_create_empty(subpool);
      stream = svn_stream_from_stringbuf(inbuf, subpool);
      len = TEST_BUF_SIZE;
      while (len == TEST_BUF_SIZE)
        {
          /* Read a chunk ... */
          SVN_ERR(svn_stream_read_full(stream, buffer, &len));

          /* ... and append the chunk to the stringbuf. */
          svn_stringbuf_appendbytes(outbuf, buffer, len);
        }

      if (! svn_stringbuf_compare(inbuf, outbuf))
        return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                                "Got unexpected result.");

      svn_pool_clear(subpool);
    }

  /* Test svn_stream_from_stringbuf() as a writable stream. */
  for (i = 0; i < NUM_TEST_STRINGS; i++)
    {
      svn_stream_t *stream;
      svn_stringbuf_t *inbuf, *outbuf;
      apr_size_t amt_read, len;

      inbuf = svn_stringbuf_create(strings[i], subpool);
      outbuf = svn_stringbuf_create_empty(subpool);
      stream = svn_stream_from_stringbuf(outbuf, subpool);
      amt_read = 0;
      while (amt_read < inbuf->len)
        {
          /* Write a chunk ... */
          len = TEST_BUF_SIZE < (inbuf->len - amt_read)
                  ? TEST_BUF_SIZE
                  : inbuf->len - amt_read;
          SVN_ERR(svn_stream_write(stream, inbuf->data + amt_read, &len));
          amt_read += len;
        }

      if (! svn_stringbuf_compare(inbuf, outbuf))
        return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                                "Got unexpected result.");

      svn_pool_clear(subpool);
    }

#undef NUM_TEST_STRINGS
#undef TEST_BUF_SIZE

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* generate some poorly compressable data */
static svn_stringbuf_t *
generate_test_bytes(int num_bytes, apr_pool_t *pool)
{
  svn_stringbuf_t *buffer = svn_stringbuf_create_empty(pool);
  int total, repeat, repeat_iter;
  char c;

  for (total = 0, repeat = repeat_iter = 1, c = 0; total < num_bytes; total++)
    {
      svn_stringbuf_appendbyte(buffer, c);

      repeat_iter--;
      if (repeat_iter == 0)
        {
          if (c == 127)
            repeat++;
          c = (char)((c + 1) % 127);
          repeat_iter = repeat;
        }
    }

  return buffer;
}


static svn_error_t *
test_stream_compressed(apr_pool_t *pool)
{
#define NUM_TEST_STRINGS 5
#define TEST_BUF_SIZE 10
#define GENERATED_SIZE 20000

  int i;
  svn_stringbuf_t *bufs[NUM_TEST_STRINGS];
  apr_pool_t *subpool = svn_pool_create(pool);

  static const char * const strings[NUM_TEST_STRINGS - 1] = {
    /* 0 */
    "",
    /* 1 */
    "This is a string.",
    /* 2 */
    "This is, by comparison to the previous string, a much longer string.",
    /* 3 */
    "And if you thought that last string was long, you just wait until "
    "I'm finished here.  I mean, how can a string really claim to be long "
    "when it fits on a single line of 80-columns?  Give me a break. "
    "Now, I'm not saying that I'm the longest string out there--far from "
    "it--but I feel that it is safe to assume that I'm far longer than my "
    "peers.  And that demands some amount of respect, wouldn't you say?"
  };


  for (i = 0; i < (NUM_TEST_STRINGS - 1); i++)
    bufs[i] = svn_stringbuf_create(strings[i], pool);

  /* the last buffer is for the generated data */
  bufs[NUM_TEST_STRINGS - 1] = generate_test_bytes(GENERATED_SIZE, pool);

  for (i = 0; i < NUM_TEST_STRINGS; i++)
    {
      svn_stream_t *stream;
      svn_stringbuf_t *origbuf, *inbuf, *outbuf;
      char buf[TEST_BUF_SIZE];
      apr_size_t len;

      origbuf = bufs[i];
      inbuf = svn_stringbuf_create_empty(subpool);
      outbuf = svn_stringbuf_create_empty(subpool);

      stream = svn_stream_compressed(svn_stream_from_stringbuf(outbuf,
                                                               subpool),
                                     subpool);
      len = origbuf->len;
      SVN_ERR(svn_stream_write(stream, origbuf->data, &len));
      SVN_ERR(svn_stream_close(stream));

      stream = svn_stream_compressed(svn_stream_from_stringbuf(outbuf,
                                                               subpool),
                                     subpool);
      len = TEST_BUF_SIZE;
      while (len >= TEST_BUF_SIZE)
        {
          len = TEST_BUF_SIZE;
          SVN_ERR(svn_stream_read_full(stream, buf, &len));
          if (len > 0)
            svn_stringbuf_appendbytes(inbuf, buf, len);
        }

      if (! svn_stringbuf_compare(inbuf, origbuf))
        return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                                "Got unexpected result.");

      SVN_ERR(svn_stream_close(stream));

      svn_pool_clear(subpool);
    }

#undef NUM_TEST_STRINGS
#undef TEST_BUF_SIZE
#undef GENERATED_SIZE

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_tee(apr_pool_t *pool)
{
  svn_stringbuf_t *test_bytes = generate_test_bytes(100, pool);
  svn_stringbuf_t *output_buf1 = svn_stringbuf_create_empty(pool);
  svn_stringbuf_t *output_buf2 = svn_stringbuf_create_empty(pool);
  svn_stream_t *source_stream = svn_stream_from_stringbuf(test_bytes, pool);
  svn_stream_t *output_stream1 = svn_stream_from_stringbuf(output_buf1, pool);
  svn_stream_t *output_stream2 = svn_stream_from_stringbuf(output_buf2, pool);
  svn_stream_t *tee_stream;

  tee_stream = svn_stream_tee(output_stream1, output_stream2, pool);
  SVN_ERR(svn_stream_copy3(source_stream, tee_stream, NULL, NULL, pool));

  if (!svn_stringbuf_compare(output_buf1, output_buf2))
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "Duplicated streams did not match.");

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_seek_file(apr_pool_t *pool)
{
  static const char *file_data[2] = {"One", "Two"};
  svn_stream_t *stream;
  svn_stringbuf_t *line;
  svn_boolean_t eof;
  apr_file_t *f;
  static const char *fname = "test_stream_seek.txt";
  int j;
  apr_status_t status;
  static const char *NL = APR_EOL_STR;
  svn_stream_mark_t *mark;

  status = apr_file_open(&f, fname, (APR_READ | APR_WRITE | APR_CREATE |
                         APR_TRUNCATE | APR_DELONCLOSE), APR_OS_DEFAULT, pool);
  if (status != APR_SUCCESS)
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL, "Cannot open '%s'",
                             fname);

  /* Create the file. */
  for (j = 0; j < 2; j++)
    {
      apr_size_t len;

      len = strlen(file_data[j]);
      status = apr_file_write(f, file_data[j], &len);
      if (status || len != strlen(file_data[j]))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "Cannot write to '%s'", fname);
      len = strlen(NL);
      status = apr_file_write(f, NL, &len);
      if (status || len != strlen(NL))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "Cannot write to '%s'", fname);
    }

  /* Create a stream to read from the file. */
  stream = svn_stream_from_aprfile2(f, FALSE, pool);
  SVN_ERR(svn_stream_reset(stream));
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(! eof && strcmp(line->data, file_data[0]) == 0);
  /* Set a mark at the beginning of the second line of the file. */
  SVN_ERR(svn_stream_mark(stream, &mark, pool));
  /* Read the second line and then seek back to the mark. */
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(! eof && strcmp(line->data, file_data[1]) == 0);
  SVN_ERR(svn_stream_seek(stream, mark));
  /* The next read should return the second line again. */
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(! eof && strcmp(line->data, file_data[1]) == 0);
  /* The next read should return EOF. */
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(eof);

  /* Go back to the beginning of the last line and try to skip it
   * NOT including the EOL. */
  SVN_ERR(svn_stream_seek(stream, mark));
  SVN_ERR(svn_stream_skip(stream, strlen(file_data[1])));
  /* The remaining line should be empty */
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(! eof && strcmp(line->data, "") == 0);
  /* The next read should return EOF. */
  SVN_ERR(svn_stream_readline(stream, &line, NL, &eof, pool));
  SVN_TEST_ASSERT(eof);

  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_seek_stringbuf(apr_pool_t *pool)
{
  svn_stream_t *stream;
  svn_stringbuf_t *stringbuf;
  char buf[4];
  apr_size_t len;
  svn_stream_mark_t *mark;

  stringbuf = svn_stringbuf_create("OneTwo", pool);
  stream = svn_stream_from_stringbuf(stringbuf, pool);
  len = 3;
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  buf[3] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "One");
  SVN_ERR(svn_stream_mark(stream, &mark, pool));
  len = 3;
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  buf[3] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "Two");
  SVN_ERR(svn_stream_seek(stream, mark));
  len = 3;
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  buf[3] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "Two");

  /* Go back to the begin of last word and try to skip some of it */
  SVN_ERR(svn_stream_seek(stream, mark));
  SVN_ERR(svn_stream_skip(stream, 2));
  /* The remaining line should be empty */
  len = 3;
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  buf[len] = '\0';
  SVN_TEST_ASSERT(len == 1);
  SVN_TEST_STRING_ASSERT(buf, "o");

  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_seek_translated(apr_pool_t *pool)
{
  svn_stream_t *stream, *translated_stream;
  svn_stringbuf_t *stringbuf;
  char buf[44]; /* strlen("One$MyKeyword: my keyword was expanded $Two") + \0 */
  apr_size_t len;
  svn_stream_mark_t *mark;
  apr_hash_t *keywords;
  svn_string_t *keyword_val;

  keywords = apr_hash_make(pool);
  keyword_val = svn_string_create("my keyword was expanded", pool);
  apr_hash_set(keywords, "MyKeyword", APR_HASH_KEY_STRING, keyword_val);
  stringbuf = svn_stringbuf_create("One$MyKeyword$Two", pool);
  stream = svn_stream_from_stringbuf(stringbuf, pool);
  translated_stream = svn_subst_stream_translated(stream, APR_EOL_STR,
                                                  FALSE, keywords, TRUE, pool);
  /* Seek from outside of keyword to inside of keyword. */
  len = 25;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 25);
  buf[25] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "One$MyKeyword: my keyword");
  SVN_ERR(svn_stream_mark(translated_stream, &mark, pool));
  SVN_ERR(svn_stream_reset(translated_stream));
  SVN_ERR(svn_stream_seek(translated_stream, mark));
  len = 4;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 4);
  buf[4] = '\0';
  SVN_TEST_STRING_ASSERT(buf, " was");

  SVN_ERR(svn_stream_seek(translated_stream, mark));
  SVN_ERR(svn_stream_skip(translated_stream, 2));
  len = 2;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 2);
  buf[len] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "as");

  /* Seek from inside of keyword to inside of keyword. */
  SVN_ERR(svn_stream_mark(translated_stream, &mark, pool));
  len = 9;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 9);
  buf[9] = '\0';
  SVN_TEST_STRING_ASSERT(buf, " expanded");
  SVN_ERR(svn_stream_seek(translated_stream, mark));
  len = 9;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 9);
  buf[9] = '\0';
  SVN_TEST_STRING_ASSERT(buf, " expanded");

  SVN_ERR(svn_stream_seek(translated_stream, mark));
  SVN_ERR(svn_stream_skip(translated_stream, 6));
  len = 3;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 3);
  buf[len] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "ded");

  /* Seek from inside of keyword to outside of keyword. */
  SVN_ERR(svn_stream_mark(translated_stream, &mark, pool));
  len = 4;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 4);
  buf[4] = '\0';
  SVN_TEST_STRING_ASSERT(buf, " $Tw");
  SVN_ERR(svn_stream_seek(translated_stream, mark));
  len = 4;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 4);
  buf[4] = '\0';
  SVN_TEST_STRING_ASSERT(buf, " $Tw");

  SVN_ERR(svn_stream_seek(translated_stream, mark));
  SVN_ERR(svn_stream_skip(translated_stream, 2));
  len = 2;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 2);
  buf[len] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "Tw");

  /* Seek from outside of keyword to outside of keyword. */
  SVN_ERR(svn_stream_mark(translated_stream, &mark, pool));
  len = 1;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 1);
  buf[1] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "o");
  SVN_ERR(svn_stream_seek(translated_stream, mark));
  len = 1;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 1);
  buf[1] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "o");

  SVN_ERR(svn_stream_seek(translated_stream, mark));
  SVN_ERR(svn_stream_skip(translated_stream, 2));
  len = 1;
  SVN_ERR(svn_stream_read_full(translated_stream, buf, &len));
  SVN_TEST_ASSERT(len == 0);
  buf[len] = '\0';
  SVN_TEST_STRING_ASSERT(buf, "");

  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_readonly(apr_pool_t *pool)
{
  const char *path;
  apr_finfo_t finfo;
  svn_boolean_t read_only;
  apr_int32_t wanted = APR_FINFO_SIZE | APR_FINFO_MTIME | APR_FINFO_TYPE
                        | APR_FINFO_LINK | APR_FINFO_PROT;


  SVN_ERR(svn_io_open_unique_file3(NULL, &path, NULL,
                                   svn_io_file_del_on_pool_cleanup,
                                   pool, pool));

  /* File should be writable */
  SVN_ERR(svn_io_stat(&finfo, path, wanted, pool));
  SVN_ERR(svn_io__is_finfo_read_only(&read_only, &finfo, pool));
  SVN_TEST_ASSERT(!read_only);

  /* Set read only */
  SVN_ERR(svn_io_set_file_read_only(path, FALSE, pool));

  /* File should be read only */
  SVN_ERR(svn_io_stat(&finfo, path, wanted, pool));
  SVN_ERR(svn_io__is_finfo_read_only(&read_only, &finfo, pool));
  SVN_TEST_ASSERT(read_only);

  /* Set writable */
  SVN_ERR(svn_io_set_file_read_write(path, FALSE, pool));

  /* File should be writable */
  SVN_ERR(svn_io_stat(&finfo, path, wanted, pool));
  SVN_ERR(svn_io__is_finfo_read_only(&read_only, &finfo, pool));
  SVN_TEST_ASSERT(!read_only);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_compressed_empty_file(apr_pool_t *pool)
{
  svn_stream_t *stream, *empty_file_stream;
  char buf[1];
  apr_size_t len;

  /* Reading an empty file with a compressed stream should not error. */
  SVN_ERR(svn_stream_open_unique(&empty_file_stream, NULL, NULL,
                                 svn_io_file_del_on_pool_cleanup,
                                 pool, pool));
  stream = svn_stream_compressed(empty_file_stream, pool);
  len = sizeof(buf);
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  if (len > 0)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "Got unexpected result.");

  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_base64(apr_pool_t *pool)
{
  svn_stream_t *stream;
  svn_stringbuf_t *actual = svn_stringbuf_create_empty(pool);
  svn_stringbuf_t *expected = svn_stringbuf_create_empty(pool);
  int i;
  static const char *strings[] = {
    "fairly boring test data... blah blah",
    "A",
    "abc",
    "012345679",
    NULL
  };

  stream = svn_stream_from_stringbuf(actual, pool);
  stream = svn_base64_decode(stream, pool);
  stream = svn_base64_encode(stream, pool);

  for (i = 0; strings[i]; i++)
    {
      apr_size_t len = strlen(strings[i]);

      svn_stringbuf_appendbytes(expected, strings[i], len);
      SVN_ERR(svn_stream_write(stream, strings[i], &len));
    }

  SVN_ERR(svn_stream_close(stream));

  SVN_TEST_STRING_ASSERT(actual->data, expected->data);

  return SVN_NO_ERROR;
}

/* This test doesn't test much unless run under valgrind when it
   triggers the problem reported here:

   http://mail-archives.apache.org/mod_mbox/subversion-dev/201202.mbox/%3C87sjik3m8q.fsf@stat.home.lan%3E

   The two data writes caused the base 64 code to allocate a buffer
   that was a byte short but exactly matched a stringbuf blocksize.
   That meant the stringbuf didn't overallocate and a write beyond
   the end of the buffer occurred.
 */
static svn_error_t *
test_stream_base64_2(apr_pool_t *pool)
{
  const struct data_t {
    const char *encoded1;
    const char *encoded2;
  } data[] = {
    {
      "MTI",
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "A23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "B23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "C23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "D23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "E23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "F23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "G23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "H23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "I23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D123456789E"
      "623456789A123456789B123456789C123456789D123456789E"
      "723456789A123456789B123456789C123456789D123456789E"
      "823456789A123456789B123456789C123456789D123456789E"
      "923456789A123456789B123456789C123456789D123456789E"
      "J23456789A123456789B123456789C123456789D123456789E"
      "123456789A123456789B123456789C123456789D123456789E"
      "223456789A123456789B123456789C123456789D123456789E"
      "323456789A123456789B123456789C123456789D123456789E"
      "423456789A123456789B123456789C123456789D123456789E"
      "523456789A123456789B123456789C123456789D12345"
    },
    {
      NULL,
      NULL,
    },
  };
  int i;

  for (i = 0; data[i].encoded1; i++)
    {
      apr_size_t len1 = strlen(data[i].encoded1);

      svn_stringbuf_t *actual = svn_stringbuf_create_empty(pool);
      svn_stringbuf_t *expected = svn_stringbuf_create_empty(pool);
      svn_stream_t *stream = svn_stream_from_stringbuf(actual, pool);

      stream = svn_base64_encode(stream, pool);
      stream = svn_base64_decode(stream, pool);

      SVN_ERR(svn_stream_write(stream, data[i].encoded1, &len1));
      svn_stringbuf_appendbytes(expected, data[i].encoded1, len1);

      if (data[i].encoded2)
        {
          apr_size_t len2 = strlen(data[i].encoded2);
          SVN_ERR(svn_stream_write(stream, data[i].encoded2, &len2));
          svn_stringbuf_appendbytes(expected, data[i].encoded2, len2);
        }

      SVN_ERR(svn_stream_close(stream));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stringbuf_from_stream(apr_pool_t *pool)
{
  const char *test_cases[] =
    {
      "",
      "x",
      "this string is longer than the default 64 minimum block size used"
      "by the function under test",
      NULL
    };

  const char **test_case;
  for (test_case = test_cases; *test_case; ++test_case)
    {
      svn_stringbuf_t *result1, *result2, *result3, *result4;
      svn_stringbuf_t *original = svn_stringbuf_create(*test_case, pool);

      svn_stream_t *stream1 = svn_stream_from_stringbuf(original, pool);
      svn_stream_t *stream2 = svn_stream_from_stringbuf(original, pool);

      SVN_ERR(svn_stringbuf_from_stream(&result1, stream1, 0, pool));
      SVN_ERR(svn_stringbuf_from_stream(&result2, stream1, 0, pool));
      SVN_ERR(svn_stringbuf_from_stream(&result3, stream2, original->len,
                                        pool));
      SVN_ERR(svn_stringbuf_from_stream(&result4, stream2, original->len,
                                        pool));

      /* C-string contents must match */
      SVN_TEST_STRING_ASSERT(result1->data, original->data);
      SVN_TEST_STRING_ASSERT(result2->data, "");
      SVN_TEST_STRING_ASSERT(result3->data, original->data);
      SVN_TEST_STRING_ASSERT(result4->data, "");

      /* assumed length must match */
      SVN_TEST_ASSERT(result1->len == original->len);
      SVN_TEST_ASSERT(result2->len == 0);
      SVN_TEST_ASSERT(result3->len == original->len);
      SVN_TEST_ASSERT(result4->len == 0);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
empty_read_full_fn(void *baton, char *buffer, apr_size_t *len)
{
    *len = 0;
    return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_compressed_read_full(apr_pool_t *pool)
{
  svn_stream_t *stream, *empty_stream;
  char buf[1];
  apr_size_t len;

  /* Reading an empty stream with read_full only support should not error. */
  empty_stream = svn_stream_create(NULL, pool);

  /* Create stream with only full read support. */
  svn_stream_set_read2(empty_stream, NULL, empty_read_full_fn);

  stream = svn_stream_compressed(empty_stream, pool);
  len = sizeof(buf);
  SVN_ERR(svn_stream_read_full(stream, buf, &len));
  if (len > 0)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "Got unexpected result.");

  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}

/* Utility function verifying that LINE contains LENGTH characters read
 * from a stream returned by create_test_read_stream().  C is the first
 * character expected in LINE. */
static svn_error_t *
expect_line_content(svn_stringbuf_t *line,
                    char start,
                    apr_size_t length)
{
  apr_size_t i;
  char c = start - 1;

  SVN_TEST_ASSERT(line->len == length);
  for (i = 0; i < length; ++i)
    {
      SVN_TEST_ASSERT(line->data[i] == c + 1);
      c = (c + 1) & 0x3f;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_stream_buffered_wrapper(apr_pool_t *pool)
{
  apr_pool_t *iterpool = svn_pool_create(pool);
  svn_stringbuf_t *line;
  svn_boolean_t eof = FALSE;
  apr_size_t read = 0;

  /* At least a few stream chunks (16k) worth of data. */
  const apr_size_t stream_length = 100000;

  /* Our source stream delivers data in very small chunks only.
   * This requires multiple reads per line while readline will hold marks
   * etc. */
  svn_stream_t *stream = create_test_read_stream(stream_length, 19, pool);
  stream = svn_stream_wrap_buffered_read(stream, pool);

  /* We told the stream not to supports seeking to the start. */
  SVN_TEST_ASSERT_ERROR(svn_stream_seek(stream, NULL),
                        SVN_ERR_STREAM_SEEK_NOT_SUPPORTED);

  /* Read all lines. Check EOF detection. */
  while (!eof)
    {
      /* The local pool ensures that marks get cleaned up. */
      svn_pool_clear(iterpool);
      SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, iterpool));

      /* Verify that we read the correct data and the full stream. */
      if (read == 0)
        SVN_ERR(expect_line_content(line, 1, '\n' - 1));
      else if (eof)
        SVN_ERR(expect_line_content(line, '\n' + 1, stream_length - read));
      else
        SVN_ERR(expect_line_content(line, '\n' + 1, 63));

      /* Update bytes read. */
      read += line->len + 1;
    }

  return SVN_NO_ERROR;
}

/* The test table.  */

static int max_threads = 1;

static struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_stream_from_string,
                   "test svn_stream_from_string"),
    SVN_TEST_PASS2(test_stream_compressed,
                   "test compressed streams"),
    SVN_TEST_PASS2(test_stream_tee,
                   "test 'tee' streams"),
    SVN_TEST_PASS2(test_stream_seek_file,
                   "test stream seeking for files"),
    SVN_TEST_PASS2(test_stream_seek_stringbuf,
                   "test stream seeking for stringbufs"),
    SVN_TEST_PASS2(test_stream_seek_translated,
                   "test stream seeking for translated streams"),
    SVN_TEST_PASS2(test_readonly,
                   "test setting a file readonly"),
    SVN_TEST_PASS2(test_stream_compressed_empty_file,
                   "test compressed streams with empty files"),
    SVN_TEST_PASS2(test_stream_base64,
                   "test base64 encoding/decoding streams"),
    SVN_TEST_PASS2(test_stream_base64_2,
                   "base64 decoding allocation problem"),
    SVN_TEST_PASS2(test_stringbuf_from_stream,
                   "test svn_stringbuf_from_stream"),
    SVN_TEST_PASS2(test_stream_compressed_read_full,
                   "test compression for streams without partial read"),
    SVN_TEST_PASS2(test_stream_buffered_wrapper,
                   "test buffering read stream wrapper"),
    SVN_TEST_NULL
  };

SVN_TEST_MAIN

// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mod_spdy/apache/spdy_output_filter.h"

#include "base/string_util.h"  // For StringToInt64

#include "mod_spdy/apache/brigade_output_stream.h"
#include "mod_spdy/apache/pool_util.h"
#include "mod_spdy/apache/response_header_populator.h"
#include "mod_spdy/common/connection_context.h"
#include "mod_spdy/common/output_filter_context.h"

namespace {

bool GetRequestStreamId(request_rec* request, spdy::SpdyStreamId *out) {
  apr_table_t* headers = request->headers_in;
  // TODO: The "x-spdy-stream-id" string really ought to be stored in a shared
  //       constant somewhere.  But where?  Anyway, that issue may be obviated
  //       if and when we find a better way to communicate the stream ID from
  //       the input filter to the output filter.
  const char* value = apr_table_get(headers, "x-spdy-stream-id");
  if (value == NULL) {
    LOG(DFATAL) << "Request had no x-spdy-stream-id header.";
    return false;
  }
  int64 id = 0;
  if (StringToInt64(value, &id)) {
    *out = static_cast<spdy::SpdyStreamId>(id);
    return true;
  }

  LOG(DFATAL) << "Couldn't parse x-spdy-stream-id: " << value;
  return false;
}

}  // namespace

namespace mod_spdy {

SpdyOutputFilter::SpdyOutputFilter(ConnectionContext* conn_context)
    : context_(new OutputFilterContext(conn_context)) {}

SpdyOutputFilter::~SpdyOutputFilter() {}

apr_status_t SpdyOutputFilter::Write(ap_filter_t* filter,
                                     apr_bucket_brigade* input_brigade) {
  // Determine whether the input brigade contains an end-of-stream bucket.
  bool is_end_of_stream = false;
  for (apr_bucket* bucket = APR_BRIGADE_FIRST(input_brigade);
       bucket != APR_BRIGADE_SENTINEL(input_brigade);
       bucket = APR_BUCKET_NEXT(bucket)) {
    if (APR_BUCKET_IS_EOS(bucket)) {
      is_end_of_stream = true;
      break;
    }
  }

  // Create an output brigade/stream.
  request_rec* const request = filter->r;
  apr_bucket_brigade* const output_brigade =
      apr_brigade_create(request->pool, request->connection->bucket_alloc);
  BrigadeOutputStream output_stream(filter, output_brigade);

  // Convert to SPDY.
  bool ok = true;
  // N.B. The sent_bodyct field is not really documented (it seems to be
  // reserved for the use of core filters) but it seems to do what we want.
  // It starts out as 0, and is set to 1 by the core HTTP_HEADER filter to
  // indicate when body data has begun to be sent.
  if (request->sent_bodyct) {
    LocalPool local;
    if (local.status() != APR_SUCCESS) {
      return local.status();
    }

    // Read all the data from the input brigade.
    char* input_data = NULL;
    apr_size_t input_size = 0;
    const apr_status_t read_status =
        apr_brigade_pflatten(input_brigade, &input_data, &input_size,
                             local.pool());
    if (read_status != APR_SUCCESS) {
      return read_status;
    }

    // Send a SPDY data frame.
    spdy::SpdyStreamId stream_id = 0;
    if (!GetRequestStreamId(request, &stream_id)) {
      return APR_EGENERAL;
    }
    ok = context_->SendData(stream_id,
                            input_data, input_size,
                            is_end_of_stream, &output_stream);
  } else if (!context_->headers_have_been_sent()) {
    spdy::SpdyStreamId stream_id = 0;
    if (!GetRequestStreamId(request, &stream_id)) {
      return APR_EGENERAL;
    }
    ResponseHeaderPopulator populator(request);
    ok = context_->SendHeaders(stream_id, populator,
                               is_end_of_stream, &output_stream);
  }

  if (is_end_of_stream) {
    APR_BRIGADE_INSERT_TAIL(
        output_brigade,
        apr_bucket_eos_create(output_brigade->bucket_alloc));
  }

  DCHECK(ok);  // TODO: Maybe we should return an error code if ok is false?

  return ap_pass_brigade(filter->next, output_brigade);
}

}  // namespace mod_spdy

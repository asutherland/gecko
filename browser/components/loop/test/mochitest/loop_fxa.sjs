/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * This is a mock server that implements the FxA endpoints on the Loop server.
 */

"use strict";

const REQUIRED_PARAMS = ["client_id", "content_uri", "oauth_uri", "profile_uri", "state"];

/**
 * Entry point for HTTP requests.
 */
function handleRequest(request, response) {
  switch (request.queryString) {
    case "/setup_params":
      setup_params(request, response);
      return;
    case "/fxa-oauth/params":
      params(request, response);
      return;
  }
  response.setStatusLine(request.httpVersion, 404, "Not Found");
}

/**
 * POST /setup_params
 * DELETE /setup_params
 *
 * Test-only endpoint to setup the /fxa-oauth/params response.
 *
 * For a POST the X-Params header should contain a JSON object with keys to set for /fxa-oauth/params.
 * A DELETE request will delete the stored parameters and should be run in a cleanup function to
 * avoid interfering with subsequen tests.
 */
function setup_params(request, response) {
  response.setHeader("Content-Type", "text/plain", false);
  if (request.method == "DELETE") {
    setSharedState("/fxa-oauth/params", "");
    response.write("Params deleted");
    return;
  }
  let params = JSON.parse(request.getHeader("X-Params"));
  if (!params) {
    response.setStatusLine(request.httpVersion, 400, "Bad Request");
    return;
  }
  setSharedState("/fxa-oauth/params", JSON.stringify(params));
  response.write("Params updated");
}

/**
 * POST /fxa-oauth/params endpoint
 *
 * Fetch OAuth parameters used to start the OAuth flow in the browser.
 * Parameters: None
 * Response: JSON containing an object of oauth parameters.
 */
function params(request, response) {
  if (request.method != "POST") {
    response.setStatusLine(request.httpVersion, 405, "Method Not Allowed");
    response.setHeader("Allow", "POST", false);

    // Add a button to make a POST request to make this endpoint easier to debug in the browser.
    response.write("<form method=POST><button type=submit>POST</button></form>");
    return;
  }

  let origin = request.scheme + "://" + request.host + ":" + request.port;

  let params = JSON.parse(getSharedState("/fxa-oauth/params") || "{}");

  // Warn if required parameters are missing.
  for (let paramName of REQUIRED_PARAMS) {
    if (!(paramName in params)) {
      dump("Warning: " + paramName + " is a required parameter\n");
    }
  }

  // Save the result so we have the effective `state` value.
  setSharedState("/fxa-oauth/params", JSON.stringify(params));
  response.setHeader("Content-Type", "application/json; charset=utf-8", false);
  response.write(JSON.stringify(params, null, 2));
}

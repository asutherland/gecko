/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceUtils_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceUtils_h

/**
 * Types/helper logic that would go in an anonymous namespace in a single file
 * but we need access to them from multiple files.  An outgrowth of our decision
 * to put much of our logic in the specific parent/child actors and those actors
 * in their own files.
 **/

namespace mozilla {
namespace dom {

// This used to have a StructuredCloneHolder class that is now mooted.  If this
// file doesn't grow some other denizen soon, nuke it.


} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceUtils_h

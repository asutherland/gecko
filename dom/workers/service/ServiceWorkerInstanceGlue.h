/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceGlue_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceGlue_h

/**
 *
 *
 **/

namespace mozilla {
namespace dom {

class ServiceWorkerInstanceConfig;

class PServiceWorkerInstanceChild;
class PServiceWorkerInstanceParent;


PServiceWorkerInstanceChild*
AllocServiceWorkerInstanceChild(const ServiceWorkerInstanceConfig& aConfig);

bool
DeallocServiceWorkerInstanceChild(PServiceWorkerInstanceChild* aActor);

PServiceWorkerInstanceParent *
AllocServiceWorkerInstanceParent();

bool
DeallocServiceWorkerInstanceParent();


} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceGlue_h

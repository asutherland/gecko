/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

PServiceWorkerEventChild*
ServiceWorkerInstanceChild::AllocPServiceWorkerEventChild(
  const ServiceWorkerEventArgs& aArgs)
{
  if (aArgs.type() != ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs &&
      aArgs.type() != ServiceWorkerEventArgs::TServiceWorkerLifeCycleEventArgs &&
      aArgs.type() != ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs &&
      aArgs.type() != ServiceWorkerEventArgs::TServiceWorkerPostMessageEventArgs) {
    MOZ_CRASH("Invalid event sent to ServiceWorkerInstance actor.");
  }

  return new ServiceWorkerEventChild(this, aArgs);
}

bool
ServiceWorkerInstanceChild::DeallocPServiceWorkerEventChild(
  PServiceWorkerEventChild* aActor)
{
  delete aActor;
  return true;
}

bool
ServiceWorkerInstanceChild::RecvPServiceWorkerEventChildConstructor(
  PServiceWorkerEventChild* aActor, const ServiceWorkerEventArgs &aArgs)
{
  return true;
}

MOZ_IMPLICIT ServiceWorkerInstanceChild::ServiceWorkerInstanceChild()
{
    MOZ_COUNT_CTOR(ServiceWorkerInstanceChild);
}

MOZ_IMPLICIT ServiceWorkerInstanceChild::~ServiceWorkerInstanceChild()
{
    MOZ_COUNT_DTOR(ServiceWorkerInstanceChild);
}

END_WORKERS_NAMESPACE

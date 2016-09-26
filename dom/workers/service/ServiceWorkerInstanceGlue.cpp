/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "dom/workers/service/ServiceWorkerInstanceGlue.h"

#include "ServiceWorkerInstanceChild.h"
#include "ServiceWorkerInstanceParent.h"

namespace mozilla {
namespace dom {

PServiceWorkerInstanceChild*
AllocServiceWorkerInstanceChild(const ServiceWorkerInstanceConfig& aConfig)
{
  nsRefPtr<ServiceWorkerInstanceChild*> actor =
    new ServiceWorkerInstanceChild(aConfig);
  return actor.forget();
}

bool
DeallocServiceWorkerInstanceChild(PServiceWorkerInstanceChild* aActor)
{
  NS_RELEASE(static_cast<ServiceWorkerInstanceChild*>(aActor));
  return true;
}

mozilla::ipc::IPCResult
InitServiceWorkerInstanceChild(PServiceWorkerInstanceChild* aActor,
                               const ServiceWorkerInstanceConfig& aConfig)
{
  auto actor = static_cast<ServiceWorkerInstanceChild*>(aActor);

  nsresult rv = actor->Init(aArgs);
  if (NS_FAILED(rv)) {
    ServiceWorkerInstanceChild::Send__delete__(
      actor, SWInstanceUnexpectedTermination(rv));
  }

  return IPC_OK();
}

PServiceWorkerInstanceParent *
AllocServiceWorkerInstanceParent()
{

}

bool
DeallocServiceWorkerInstanceParent()
{

}

} // namespace dom
} // namespace mozilla

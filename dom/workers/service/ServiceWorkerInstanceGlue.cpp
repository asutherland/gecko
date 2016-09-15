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
  return new ServiceWorkerInstanceChild(aConfig);
}

bool
DeallocServiceWorkerInstanceChild(PServiceWorkerInstanceChild* aActor)
{
  delete aActor;
  return true;
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

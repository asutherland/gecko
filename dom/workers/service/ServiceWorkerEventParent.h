/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerEventParent_h
#define mozilla_dom_workers_service_ServiceWorkerEventParent_h


namespace mozilla {
namespace dom {

namespace workers {

/**
 * Holds event callback and keepalive tokens for ServiceWorkerPrivate.
 * ServiceWorkerPrivate does no other per-event bookkeeping.
 */
class ServiceWorkerEventParent final : public PServiceWorkerEventParent
{

public:
  ServiceWorkerEventParent(LifeCycleEventCallback* aCallback);
  ~ServiceWorkerEventParent();

private:
  // PServiceWorkerEvent methods
  virtual bool
  RecvFetchEventRespondWith(const IPCInternalResponse& aResponse) override;

  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  nsRefPtr<LifeCycleEventCallback> mCallback;
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerEventParent_h

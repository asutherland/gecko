/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerEventChild_h
#define mozilla_dom_workers_service_ServiceWorkerEventChild_h


namespace mozilla {
namespace dom {

namespace workers {

/**
 * Home to the runnables/logic for servicing the events; our owning
 * ServiceWorkerInstanceChild owns the WorkerPrivate.  We are similarly
 * reference counted like it because of all the runnables involved in the use
 * of workers.
 */
class ServiceWorkerEventChild final : public PServiceWorkerEventChild
                                    , public nsISupports
{
public:
  ServiceWorkerEventChild();
  ~ServiceWorkerEventChild();

private:
  // PServiceWorkerInstance methods
  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  RefPtr<ServiceWorkerInstanceChild> mOwner;

public:
  NS_DECL_ISUPPORTS
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerEventChild_h

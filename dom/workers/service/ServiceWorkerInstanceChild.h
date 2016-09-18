/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceChild_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceChild_h

namespace mozilla {
namespace dom {

namespace workers {

/**
 * Creates and holds a ServiceWorker WorkerPrivate active for remote events to
 * be dispatched to it.  The ServiceWorkerEventChild implementation is where the
 * event arguments are interpreted and converted into the runnables that
 * interact with the worker thread.
 */
class ServiceWorkerInstanceChild final : public PServiceWorkerInstanceChild
{
public:


private:
  // PServiceWorkerInstance methods
  virtual PServiceWorkerEventChild*
  AllocPServiceWorkerEventChild(const ServiceWorkerEventArgs& aArgs) override;

  virtual bool
  DeallocPServiceWorkerEventChild(PServiceWorkerEventChild* aActor) override;

  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  // XXX ipdl generated stuffs
  MOZ_IMPLICIT ServiceWorkerInstanceChild();
  virtual ~ServiceWorkerInstanceChild();
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceChild_h

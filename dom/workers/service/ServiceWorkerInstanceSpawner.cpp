/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerInstanceSpawner.h"

#include "ServiceWorkerInfo.h"
#include "ServiceWorkerPrivate.h"
#include "Workers.h" // For AssertIsOnMainThread

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

#define IPC_CONTENT_SHUTDOWN "ipc:content-shutdown"

static StaticRefPtr<ServiceWorkerInstanceSpawner> gInstance;
/**
 * Since we're spawned only on first demand right now, a static at-most-once
 * guard inside SpawnInstance is insufficient to avoid us being spawned during
 * shutdown.  So we use a compilation-unit-level static.
 */
static gShutdownIssued = false;

/* static */ ServiceWorkerInstanceParent*
ServiceWorkerInstanceSpawner::SpawnInstance(ServiceWorkerPrivate* aOwner)
{
  AssertIsOnMainThread();

  if (!gInstance)
    if (!gShutdownIssued) {
      gInstance = new ServiceWorkerInstanceSpawner();
      gInstance->Init();
      ClearOnShutdown(&gInstance);
    } else {
      return nullptr;
    }
  }

  return gInstance->Spawn(aOwner);
}

/* static */
ServiceWorkerInstanceSpawner::Shutdown()
{
  gShutdownIssued = true;

  if (!gInstance) {
    return;
  }

  gInstance->Shutdown();
}

// nsISupports
NS_IMPL_ISUPPORTS(ServiceWorkerInstanceSpawner, nsIObserver)

// nsIObserver
NS_IMETHODIMP
ServiceWorkerInstanceSpawner::Observe(nsISupports* aSubject, const char* aTopic,
                                      const char16_t* aData)
{
  if (strcmp(aTopic, IPC_CONTENT_SHUTDOWN) == 0) {
    nsCOMPtr<nsIPropertyBag2> props = do_QueryInterface(aSubject);
    if (!props) {
      NS_WARNING("ipc:content-shutdown message without property bag subject");
      return NS_OK;
    }

    uint64_t childID = 0;
    nsresult rv = props->GetPropertyAsUint64(NS_LITERAL_STRING("childID"),
                                             &childID);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (mDedicatedServiceWorkerProcess->ChildID() != childID) {
      return NS_OK;
    }

    // It was our process, drop the reference.
    mDedicatedServiceWorkerProcess = nullptr;

    // If the process was abnormally terminated and we're not trying to
    // shutdown, spin up a new process to process future events.  Existing
    // events will die without any form of automated retry because the only
    // things in the process are ServiceWorkers and we can expect some amount of
    // failure correlation.  Specifically, code that triggers a bug has a
    // non-trivial chance of triggering the bug again.  And resource exhaustion
    // (like memory), is probably still going to be a problem in the short term.
    bool abnormal;
    rv = props->GetPropertyAsBool(NS_LITERAL_STRING("abnormal"), &abnormal);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    if (abnormal && !gShutdownIssued) {
      mDedicatedServiceWorkerProcess =
        ContentParent::ProvideFreshContentParent();
    } else if (!abnormal && !gShutdownIssued) {
      // If this happens, someone else broke us by enumerating our process and
      // killing it.
      NS_WARNING("Someone else shutdown the dedicated ServiceWorker process?!");
    }

    return NS_OK;
  }

  MOZ_CRASH("Received message we aren't supposed to be registered for!");
  return NS_OK;
}

ServiceWorkerInstanceSpawner::ServiceWorkerInstanceSpawner()
  : mDedicatedServiceWorkerProcess(nullptr)
{
}

ServiceWorkerInstanceSpawner::~ServiceWorkerInstanceSpawner()
{
}

void
ServiceWorkerInstanceSpawner::Init()
{
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    DebugOnly<nsresult> rv;
    rv = obs->AddObserver(this, IPC_CONTENT_SHUTDOWN, false /* ownsWeak */);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  mDedicatedServiceWorkerProcess = ContentParent::ProvideFreshContentParent();
}

ServiceWorkerInstanceParent*
ServiceWorkerInstanceSpawner::Spawn(ServiceWorkerPrivate* aOwner)
{
  MOZ_ASSERT(aOwner);
  MOZ_ASSERT(aOwner->mInfo);

  if (!mDedicatedServiceWorkerProcess) {
    return nullptr;
  }

  ServiceWorkerInfo &info = *aOwner->mInfo;

  ServiceWorkerInstanceConfig config;
  config.instanceID() = info.ID();
  config.scope() = info.Scope();
  config.currentWorkerURL() = info.ScriptSpec();
  config.cacheName() = info.CacheName();
  mozilla::ipc::PrincipalToPrincipalInfo(info.GetPrincipal(),
                                         &config.principal());

  ServiceWorkerInstanceParent* actor = new ServiceWorkerInstanceParent();
  // In event of failure it will deallocate the actor and return nullptr,
  // otherwise it returns our actor which we need to downcast to impl again.
  return static_cast<ServiceWorkerInstanceParent*>(
    mDedicatedServiceWorkerProcess->SendPServiceWorkerInstanceConstructor(
      actor, config));
}

void
ServiceWorkerInstanceSpawner::Shutdown()
{
  if (mDedicatedServiceWorkerProcess) {
    // Tell the child it's doomed so it gets a RecvShutdown and the profiler,
    // if active, gets a chance to send us its data before it dies.  We're not
    // concerned about the impact on any live ServiceWorkers since they are
    // oblivious to shutdown and things like push notifications should not be
    // considering a notification delivered if the event does not run to
    // completion.
    mDedicatedServiceWorkerProcess->ShutDownProcess(
      ContentParent::SEND_SHUTDOWN_MESSAGE);
    // We'll drop the reference in our "ipc:content-shutdown" observer listener
    // which gets invoked by ContentParent::ActorDestroy.
  }
}

END_WORKERS_NAMESPACE

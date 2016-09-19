/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

void
ServiceWorkerInstanceChild::Init(const ServiceWorkerInstanceConfig& aConfig)
{
  AssertIsOnMainThread();

  // Ensure that the IndexedDatabaseManager is initialized
  Unused << NS_WARN_IF(!IndexedDatabaseManager::GetOrCreate());

  WorkerLoadInfo info;
  nsresult rv = NS_NewURI(getter_AddRefs(info.mBaseURI), mInfo->ScriptSpec(),
                          nullptr, nullptr);

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  info.mResolvedScriptURI = info.mBaseURI;
  MOZ_ASSERT(!mInfo->CacheName().IsEmpty());
  info.mServiceWorkerCacheName = mInfo->CacheName();
  info.mServiceWorkerID = mInfo->ID();
  info.mLoadGroup = aLoadGroup;
  info.mLoadFailedAsyncRunnable = aLoadFailedRunnable;

  // If we are loading a script for a ServiceWorker then we must not
  // try to intercept it.  If the interception matches the current
  // ServiceWorker's scope then we could deadlock the load.
  info.mLoadFlags = mInfo->GetLoadFlags() |
                    nsIChannel::LOAD_BYPASS_SERVICE_WORKER;

  rv = info.mBaseURI->GetHost(info.mDomain);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  info.mPrincipal = mInfo->GetPrincipal();

  nsContentUtils::StorageAccess access =
    nsContentUtils::StorageAllowedForPrincipal(info.mPrincipal);
  info.mStorageAllowed = access > nsContentUtils::StorageAccess::ePrivateBrowsing;
  info.mOriginAttributes = mInfo->GetOriginAttributes();

  nsCOMPtr<nsIContentSecurityPolicy> csp;
  rv = info.mPrincipal->GetCsp(getter_AddRefs(csp));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  info.mCSP = csp;
  if (info.mCSP) {
    rv = info.mCSP->GetAllowsEval(&info.mReportCSPViolations,
                                  &info.mEvalAllowed);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  } else {
    info.mEvalAllowed = true;
    info.mReportCSPViolations = false;
  }

  WorkerPrivate::OverrideLoadInfoLoadGroup(info);

  AutoJSAPI jsapi;
  jsapi.Init();
  ErrorResult error;
  NS_ConvertUTF8toUTF16 scriptSpec(mInfo->ScriptSpec());

  mWorkerPrivate = WorkerPrivate::Constructor(jsapi.cx(),
                                              scriptSpec,
                                              false, WorkerTypeService,
                                              mInfo->Scope(), &info, error);
  if (NS_WARN_IF(error.Failed())) {
    return error.StealNSResult();
  }
}

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

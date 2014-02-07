// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/translate/translate_service.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/translate/translate_manager.h"
#include "components/translate/core/browser/translate_download_manager.h"

namespace {
// The singleton instance of TranslateService.
TranslateService* g_translate_service = NULL;
}

TranslateService::TranslateService() {
  resource_request_allowed_notifier_.Init(this);
}

TranslateService::~TranslateService() {}

// static
void TranslateService::Initialize() {
  if (g_translate_service)
    return;

  g_translate_service = new TranslateService;
  // Initialize the allowed state for resource requests.
  g_translate_service->OnResourceRequestsAllowed();
  // Create the TranslateManager singleton.
  TranslateManager::GetInstance();
  TranslateDownloadManager* download_manager =
      TranslateDownloadManager::GetInstance();
  download_manager->set_request_context(
      g_browser_process->system_request_context());
  download_manager->set_application_locale(
      g_browser_process->GetApplicationLocale());
}

// static
void TranslateService::Shutdown(bool cleanup_pending_fetcher) {
  TranslateDownloadManager* download_manager =
      TranslateDownloadManager::GetInstance();
  if (cleanup_pending_fetcher) {
    download_manager->Shutdown();
  } else {
    // This path is only used by tests.
    download_manager->set_request_context(NULL);
  }
}

void TranslateService::OnResourceRequestsAllowed() {
  TranslateLanguageList* language_list =
      TranslateDownloadManager::GetInstance()->language_list();
  if (!language_list) {
    NOTREACHED();
    return;
  }

  language_list->SetResourceRequestsAllowed(
      resource_request_allowed_notifier_.ResourceRequestsAllowed());
}

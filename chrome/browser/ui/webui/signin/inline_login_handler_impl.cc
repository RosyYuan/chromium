// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/signin/inline_login_handler_impl.h"

#include "base/bind.h"
#include "base/prefs/pref_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/signin_global_error.h"
#include "chrome/browser/signin/signin_oauth_helper.h"
#include "chrome/browser/signin/signin_promo.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/sync/one_click_signin_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "google_apis/gaia/gaia_auth_fetcher.h"
#include "google_apis/gaia/gaia_constants.h"
#include "google_apis/gaia/gaia_urls.h"
#include "net/base/url_util.h"

namespace {

} // empty namespace

InlineLoginHandlerImpl::InlineLoginHandlerImpl()
      : weak_factory_(this), choose_what_to_sync_(false) {
}

InlineLoginHandlerImpl::~InlineLoginHandlerImpl() {}

void InlineLoginHandlerImpl::RegisterMessages() {
  InlineLoginHandler::RegisterMessages();

  web_ui()->RegisterMessageCallback("switchToFullTab",
      base::Bind(&InlineLoginHandlerImpl::HandleSwitchToFullTabMessage,
                  base::Unretained(this)));
}

void InlineLoginHandlerImpl::SetExtraInitParams(base::DictionaryValue& params) {
  params.SetInteger("authMode", InlineLoginHandler::kDesktopAuthMode);

  const GURL& current_url = web_ui()->GetWebContents()->GetURL();
  signin::Source source = signin::GetSourceForPromoURL(current_url);
  DCHECK(source != signin::SOURCE_UNKNOWN);
  if (source == signin::SOURCE_AVATAR_BUBBLE_ADD_ACCOUNT ||
      source == signin::SOURCE_AVATAR_BUBBLE_SIGN_IN) {
    // Drop the leading slash in the path.
    params.SetString("gaiaPath",
        GaiaUrls::GetInstance()->embedded_signin_url().path().substr(1));
  }

  params.SetString("service", "chromiumsync");
  params.SetString("continueUrl",
      signin::GetLandingURL("source", static_cast<int>(source)).spec());

  std::string default_email;
  if (source != signin::SOURCE_AVATAR_BUBBLE_ADD_ACCOUNT) {
    default_email = Profile::FromWebUI(web_ui())->GetPrefs()->
        GetString(prefs::kGoogleServicesLastUsername);
  } else {
    if (!net::GetValueForKeyInQuery(current_url, "email", &default_email))
      default_email.clear();
  }
  if (!default_email.empty())
    params.SetString("email", default_email);

  std::string frame_url;
  net::GetValueForKeyInQuery(current_url, "frameUrl", &frame_url);
  if (!frame_url.empty())
    params.SetString("frameUrl", frame_url);

  std::string is_constrained;
  net::GetValueForKeyInQuery(current_url, "constrained", &is_constrained);
  if (!is_constrained.empty())
    params.SetString("constrained", is_constrained);

  // TODO(rogerta): this needs to be passed on to gaia somehow.
  std::string read_only_email;
  net::GetValueForKeyInQuery(current_url, "readOnlyEmail", &read_only_email);
  if (!read_only_email.empty())
    params.SetString("readOnlyEmail", read_only_email);
}


void InlineLoginHandlerImpl::HandleSwitchToFullTabMessage(
    const base::ListValue* args) {
  base::string16 url_str;
  CHECK(args->GetString(0, &url_str));

  content::WebContents* web_contents = web_ui()->GetWebContents();
  GURL main_frame_url(web_contents->GetURL());
  main_frame_url = net::AppendOrReplaceQueryParameter(
      main_frame_url, "frameUrl", UTF16ToASCII(url_str));
  chrome::NavigateParams params(
      Profile::FromWebUI(web_ui()),
      net::AppendOrReplaceQueryParameter(main_frame_url, "constrained", "0"),
      content::PAGE_TRANSITION_AUTO_TOPLEVEL);
  chrome::Navigate(&params);

  web_ui()->CallJavascriptFunction("inline.login.closeDialog");
}

void InlineLoginHandlerImpl::CompleteLogin(const base::ListValue* args) {
  DCHECK(email_.empty() && password_.empty() && session_index_.empty());

  content::WebContents* contents = web_ui()->GetWebContents();
  const GURL& current_url = contents->GetURL();

  const base::DictionaryValue* dict = NULL;
  base::string16 email;
  if (!args->GetDictionary(0, &dict) || !dict ||
      !dict->GetString("email", &email)) {
    // User cancelled the signin by clicking 'skip for now'.
    bool skip_for_now = false;
    DCHECK(dict->GetBoolean("skipForNow", &skip_for_now) && skip_for_now);

    signin::SetUserSkippedPromo(Profile::FromWebUI(web_ui()));
    SyncStarterCallback(OneClickSigninSyncStarter::SYNC_SETUP_FAILURE);
    return;
  }

  email_ = UTF16ToASCII(email);
  base::string16 password;
  dict->GetString("password", &password);
  password_ = UTF16ToASCII(password);

  // When doing a SAML sign in, this email check may result in a false
  // positive.  This happens when the user types one email address in the
  // gaia sign in page, but signs in to a different account in the SAML sign in
  // page.
  std::string default_email;
  std::string validate_email;
  if (net::GetValueForKeyInQuery(current_url, "email", &default_email) &&
      net::GetValueForKeyInQuery(current_url, "validateEmail",
                                 &validate_email) &&
      validate_email == "1") {
    if (email_ != default_email) {
      SyncStarterCallback(OneClickSigninSyncStarter::SYNC_SETUP_FAILURE);
      return;
    }
  }

  base::string16 session_index;
  dict->GetString("sessionIndex", &session_index);
  session_index_ = UTF16ToASCII(session_index);
  DCHECK(!session_index_.empty());
  dict->GetBoolean("chooseWhatToSync", &choose_what_to_sync_);

  signin::Source source = signin::GetSourceForPromoURL(current_url);
  OneClickSigninHelper::CanOfferFor can_offer =
      source == signin::SOURCE_AVATAR_BUBBLE_ADD_ACCOUNT ?
      OneClickSigninHelper::CAN_OFFER_FOR_SECONDARY_ACCOUNT :
      OneClickSigninHelper::CAN_OFFER_FOR_ALL;
  std::string error_msg;
  OneClickSigninHelper::CanOffer(
      contents, can_offer, email_, &error_msg);
  if (!error_msg.empty()) {
    HandleLoginError(error_msg);
    return;
  }

  content::StoragePartition* partition =
      content::BrowserContext::GetStoragePartitionForSite(
          contents->GetBrowserContext(),
          GURL(chrome::kChromeUIChromeSigninURL));

  auth_fetcher_.reset(new GaiaAuthFetcher(this,
                                          GaiaConstants::kChromeSource,
                                          partition->GetURLRequestContext()));
  auth_fetcher_->StartCookieForOAuthCodeExchange(session_index_);
}

void InlineLoginHandlerImpl::OnClientOAuthCodeSuccess(
    const std::string& oauth_code) {
  DCHECK(!oauth_code.empty());

  content::WebContents* contents = web_ui()->GetWebContents();
  Profile* profile = Profile::FromWebUI(web_ui());
  ProfileSyncService* sync_service =
      ProfileSyncServiceFactory::GetForProfile(profile);
  const GURL& current_url = contents->GetURL();
  signin::Source source = signin::GetSourceForPromoURL(current_url);

  if (source == signin::SOURCE_AVATAR_BUBBLE_ADD_ACCOUNT) {
    // SigninOAuthHelper will delete itself.
    SigninOAuthHelper* helper = new SigninOAuthHelper(profile);
    helper->StartAddingAccount(oauth_code);

    if (signin::IsAutoCloseEnabledInURL(current_url)) {
      // Close the gaia sign in tab via a task to make sure we aren't in the
      // middle of any webui handler code.
      base::MessageLoop::current()->PostTask(
          FROM_HERE,
          base::Bind(&InlineLoginHandlerImpl::CloseTab,
                     weak_factory_.GetWeakPtr()));
    }
  } else {
    OneClickSigninSyncStarter::StartSyncMode start_mode =
        source == signin::SOURCE_SETTINGS || choose_what_to_sync_ ?
            (SigninGlobalError::GetForProfile(profile)->HasMenuItem() &&
              sync_service && sync_service->HasSyncSetupCompleted()) ?
                OneClickSigninSyncStarter::SHOW_SETTINGS_WITHOUT_CONFIGURE :
                OneClickSigninSyncStarter::CONFIGURE_SYNC_FIRST :
            OneClickSigninSyncStarter::SYNC_WITH_DEFAULT_SETTINGS;
    OneClickSigninSyncStarter::ConfirmationRequired confirmation_required =
        source == signin::SOURCE_SETTINGS ||
        source == signin::SOURCE_WEBSTORE_INSTALL ||
        choose_what_to_sync_?
            OneClickSigninSyncStarter::NO_CONFIRMATION :
            OneClickSigninSyncStarter::CONFIRM_AFTER_SIGNIN;
      OneClickSigninSyncStarter::Callback sync_callback = base::Bind(
          &InlineLoginHandlerImpl::SyncStarterCallback,
          weak_factory_.GetWeakPtr());

      bool cross_account_error_handled =
          OneClickSigninHelper::HandleCrossAccountError(
              contents, "" /* session_index, not used */,
              email_, password_, oauth_code,
              OneClickSigninHelper::AUTO_ACCEPT_EXPLICIT,
              source, start_mode, sync_callback);

      if (!cross_account_error_handled) {
        // Call OneClickSigninSyncStarter to exchange oauth code for tokens.
        // OneClickSigninSyncStarter will delete itself once the job is done.
        new OneClickSigninSyncStarter(
            profile, NULL, "" /* session_index, not used */,
            email_, password_, oauth_code,
            start_mode,
            contents,
            confirmation_required,
            sync_callback);
      }
  }

  email_.clear();
  password_.clear();
  session_index_.clear();
  web_ui()->CallJavascriptFunction("inline.login.closeDialog");
}

void InlineLoginHandlerImpl::OnClientOAuthCodeFailure(
    const GoogleServiceAuthError& error) {
  LOG(ERROR) << "InlineLoginUI::OnClientOAuthCodeFailure";
  HandleLoginError(error.ToString());
}

void InlineLoginHandlerImpl::HandleLoginError(const std::string& error_msg) {
  SyncStarterCallback(OneClickSigninSyncStarter::SYNC_SETUP_FAILURE);

  Browser* browser = chrome::FindBrowserWithWebContents(
      web_ui()->GetWebContents());
  if (!browser) {
    browser = chrome::FindLastActiveWithProfile(
        Profile::FromWebUI(web_ui()), chrome::GetActiveDesktop());
  }
  if (browser)
    OneClickSigninHelper::ShowSigninErrorBubble(browser, error_msg);

  email_.clear();
  password_.clear();
  session_index_.clear();
}

void InlineLoginHandlerImpl::SyncStarterCallback(
    OneClickSigninSyncStarter::SyncSetupResult result) {
  content::WebContents* contents = web_ui()->GetWebContents();
  const GURL& current_url = contents->GetURL();
  bool auto_close = signin::IsAutoCloseEnabledInURL(current_url);
  if (auto_close) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(&InlineLoginHandlerImpl::CloseTab,
                   weak_factory_.GetWeakPtr()));
  } else {
     signin::Source source = signin::GetSourceForPromoURL(current_url);
     DCHECK(source != signin::SOURCE_UNKNOWN);
     OneClickSigninHelper::RedirectToNtpOrAppsPageIfNecessary(contents, source);
  }
}

void InlineLoginHandlerImpl::CloseTab() {
  content::WebContents* tab = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithWebContents(tab);
  if (browser) {
    TabStripModel* tab_strip_model = browser->tab_strip_model();
    if (tab_strip_model) {
      int index = tab_strip_model->GetIndexOfWebContents(tab);
      if (index != TabStripModel::kNoTab) {
        tab_strip_model->ExecuteContextMenuCommand(
            index, TabStripModel::CommandCloseTab);
      }
    }
  }
}

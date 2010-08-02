// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome_frame/test/ie_event_sink.h"

#include "base/scoped_bstr_win.h"
#include "base/scoped_handle.h"
#include "base/string_util.h"
#include "base/utf_string_conversions.h"
#include "base/win_util.h"
#include "chrome_frame/test/chrome_frame_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chrome_frame_test {

const int kDefaultWaitForIEToTerminateMs = 10 * 1000;

_ATL_FUNC_INFO IEEventSink::kNavigateErrorInfo = {
  CC_STDCALL, VT_EMPTY, 5, {
    VT_DISPATCH,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_BOOL | VT_BYREF,
  }
};

_ATL_FUNC_INFO IEEventSink::kNavigateComplete2Info = {
  CC_STDCALL, VT_EMPTY, 2, {
    VT_DISPATCH,
    VT_VARIANT | VT_BYREF
  }
};

_ATL_FUNC_INFO IEEventSink::kBeforeNavigate2Info = {
  CC_STDCALL, VT_EMPTY, 7, {
    VT_DISPATCH,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_VARIANT | VT_BYREF,
    VT_BOOL | VT_BYREF
  }
};

_ATL_FUNC_INFO IEEventSink::kNewWindow2Info = {
  CC_STDCALL, VT_EMPTY, 2, {
    VT_DISPATCH | VT_BYREF,
    VT_BOOL | VT_BYREF,
  }
};

_ATL_FUNC_INFO IEEventSink::kNewWindow3Info = {
  CC_STDCALL, VT_EMPTY, 5, {
    VT_DISPATCH | VT_BYREF,
    VT_BOOL | VT_BYREF,
    VT_UINT,
    VT_BSTR,
    VT_BSTR
  }
};

_ATL_FUNC_INFO IEEventSink::kVoidMethodInfo = {
    CC_STDCALL, VT_EMPTY, 0, {NULL}};

_ATL_FUNC_INFO IEEventSink::kDocumentCompleteInfo = {
  CC_STDCALL, VT_EMPTY, 2, {
    VT_DISPATCH,
    VT_VARIANT | VT_BYREF
  }
};

_ATL_FUNC_INFO IEEventSink::kFileDownloadInfo = {
  CC_STDCALL, VT_EMPTY, 2, {
    VT_BOOL,
    VT_BOOL | VT_BYREF
  }
};

IEEventSink::IEEventSink()
    : ALLOW_THIS_IN_INITIALIZER_LIST(
          onmessage_(this, &IEEventSink::OnMessage)),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          onloaderror_(this, &IEEventSink::OnLoadError)),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          onload_(this, &IEEventSink::OnLoad)),
      listener_(NULL),
      ie_process_id_(0),
      did_receive_on_quit_(false) {
}

IEEventSink::~IEEventSink() {
  Uninitialize();
}

// IEEventSink member defines
void IEEventSink::Attach(IDispatch* browser_disp) {
  EXPECT_TRUE(NULL != browser_disp);
  if (browser_disp) {
    EXPECT_HRESULT_SUCCEEDED(web_browser2_.QueryFrom(browser_disp));
    EXPECT_TRUE(S_OK == DispEventAdvise(web_browser2_,
                                        &DIID_DWebBrowserEvents2));
    FindIEProcessId();
  }
}

void IEEventSink::Uninitialize() {
  DisconnectFromChromeFrame();
  if (web_browser2_.get()) {
    if (m_dwEventCookie != 0xFEFEFEFE) {
      DispEventUnadvise(web_browser2_);
      CoDisconnectObject(this, 0);
    }

    if (!did_receive_on_quit_) {
      // Log the browser window url for debugging purposes.
      ScopedBstr browser_url;
      web_browser2_->get_LocationURL(browser_url.Receive());
      std::wstring browser_url_wstring;
      browser_url_wstring.assign(browser_url, browser_url.Length());
      std::string browser_url_string = WideToUTF8(browser_url_wstring);
      EXPECT_TRUE(did_receive_on_quit_) << "OnQuit was not received for "
                                        << "browser with url "
                                        << browser_url_string;

      web_browser2_->Quit();
    }

    ScopedHandle process;
    process.Set(OpenProcess(SYNCHRONIZE, FALSE, ie_process_id_));
    DLOG_IF(WARNING, !process.IsValid())
        << StringPrintf("OpenProcess failed: %i", ::GetLastError());

    web_browser2_.Release();

    // IE may not have closed yet. Wait here for the process to finish.
    // This is necessary at least on some browser/platform configurations.
    if (process) {
      DWORD max_wait = kDefaultWaitForIEToTerminateMs;
      while (true) {
        base::Time start = base::Time::Now();
        HANDLE wait_for = process;
        DWORD wait = MsgWaitForMultipleObjects(1, &wait_for, FALSE, max_wait,
                                               QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 + 1) {
          MSG msg;
          while (PeekMessage(&msg, NULL, 0, 0, TRUE) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
          }
        } else if (wait == WAIT_OBJECT_0) {
          break;
        } else {
          DCHECK(wait == WAIT_TIMEOUT);
          DLOG(ERROR) << "Wait for IE timed out";
          break;
        }
        base::TimeDelta elapsed = base::Time::Now() - start;
        ULARGE_INTEGER ms;
        ms.QuadPart = elapsed.InMilliseconds();
        DCHECK_EQ(ms.HighPart, 0U);
        if (ms.LowPart > max_wait) {
          DLOG(ERROR) << "Wait for IE timed out (2)";
          break;
        } else {
          max_wait -= ms.LowPart;
        }
      }
    }
  }
}

bool IEEventSink::IsCFRendering() {
  DCHECK(web_browser2_);

  HWND renderer_window;
  if (web_browser2_) {
    ScopedComPtr<IDispatch> doc;
    web_browser2_->get_Document(doc.Receive());
    if (doc) {
      ScopedComPtr<IOleWindow> ole_window;
      ole_window.QueryFrom(doc);
      if (ole_window) {
        ole_window->GetWindow(&renderer_window);
        if (IsWindow(renderer_window)) {
          wchar_t class_name[MAX_PATH] = {0};
          GetClassName(renderer_window, class_name, arraysize(class_name));
          return _wcsicmp(class_name, L"Internet Explorer_Server") != 0;
        }
      }
    }
  }
  return false;
}

void IEEventSink::SetFocusToRenderer() {
  simulate_input::SetKeyboardFocusToWindow(GetRendererWindow());
}

void IEEventSink::SendKeys(const wchar_t* input_string) {
  SetFocusToRenderer();
  simulate_input::SendStringW(input_string);
}

void IEEventSink::SendMouseClick(int x, int y,
                                 simulate_input::MouseButton button) {
  simulate_input::SendMouseClick(GetRendererWindow(), x, y, button);
}

void IEEventSink::ExpectRendererWindowHasFocus() {
  HWND renderer_window = GetRendererWindow();
  EXPECT_TRUE(IsWindow(renderer_window));

  DWORD renderer_thread = 0;
  DWORD renderer_process = 0;
  renderer_thread = GetWindowThreadProcessId(renderer_window,
                                             &renderer_process);

  ASSERT_TRUE(AttachThreadInput(GetCurrentThreadId(), renderer_thread, TRUE));
  HWND focus_window = GetFocus();
  EXPECT_TRUE(focus_window == renderer_window);
  EXPECT_TRUE(AttachThreadInput(GetCurrentThreadId(), renderer_thread, FALSE));
}

void IEEventSink::ExpectAddressBarUrl(
    const std::wstring& expected_url) {
  DCHECK(web_browser2_);
  if (web_browser2_) {
    ScopedBstr address_bar_url;
    EXPECT_EQ(S_OK, web_browser2_->get_LocationURL(address_bar_url.Receive()));
    EXPECT_EQ(expected_url, std::wstring(address_bar_url));
  }
}

void IEEventSink::Exec(const GUID* cmd_group_guid, DWORD command_id,
                               DWORD cmd_exec_opt, VARIANT* in_args,
                               VARIANT* out_args) {
  ScopedComPtr<IOleCommandTarget> shell_browser_cmd_target;
  DoQueryService(SID_STopLevelBrowser, web_browser2_,
                 shell_browser_cmd_target.Receive());
  ASSERT_TRUE(NULL != shell_browser_cmd_target);
  EXPECT_HRESULT_SUCCEEDED(shell_browser_cmd_target->Exec(cmd_group_guid,
      command_id, cmd_exec_opt, in_args, out_args));
}

HWND IEEventSink::GetRendererWindow() {
  HWND renderer_window = NULL;
  if (IsCFRendering()) {
    DCHECK(chrome_frame_);
    ScopedComPtr<IOleWindow> ole_window;
    ole_window.QueryFrom(chrome_frame_);
    EXPECT_TRUE(ole_window.get());

    if (ole_window) {
      HWND activex_window = NULL;
      ole_window->GetWindow(&activex_window);
      EXPECT_TRUE(IsWindow(activex_window));

      // chrome tab window is the first (and the only) child of activex
      for (HWND first_child = activex_window;
        IsWindow(first_child); first_child = GetWindow(first_child, GW_CHILD)) {
        renderer_window = first_child;
      }
      wchar_t class_name[MAX_PATH] = {0};
      GetClassName(renderer_window, class_name, arraysize(class_name));
      EXPECT_EQ(0, _wcsicmp(class_name, L"Chrome_RenderWidgetHostHWND"));
    }
  } else {
    DCHECK(web_browser2_);
    ScopedComPtr<IDispatch> doc;
    HRESULT hr = web_browser2_->get_Document(doc.Receive());
    EXPECT_HRESULT_SUCCEEDED(hr);
    EXPECT_TRUE(doc);
    if (doc) {
      ScopedComPtr<IOleWindow> ole_window;
      ole_window.QueryFrom(doc);
      EXPECT_TRUE(ole_window);
      if (ole_window) {
        ole_window->GetWindow(&renderer_window);
      }
    }
  }

  EXPECT_TRUE(IsWindow(renderer_window));
  return renderer_window;
}

HRESULT IEEventSink::LaunchIEAndNavigate(
    const std::wstring& navigate_url, IEEventListener* listener) {
  listener_ = listener;
  HRESULT hr = LaunchIEAsComServer(web_browser2_.Receive());
  EXPECT_EQ(S_OK, hr);
  if (hr == S_OK) {
    FindIEProcessId();
    web_browser2_->put_Visible(VARIANT_TRUE);
    hr = DispEventAdvise(web_browser2_, &DIID_DWebBrowserEvents2);
    EXPECT_TRUE(hr == S_OK);
    hr = Navigate(navigate_url);
  }

  DLOG_IF(WARNING, FAILED(hr)) << "Failed to launch IE. Error:" << hr;
  return hr;
}

HRESULT IEEventSink::Navigate(const std::wstring& navigate_url) {
  VARIANT empty = ScopedVariant::kEmptyVariant;
  ScopedVariant url;
  url.Set(navigate_url.c_str());

  HRESULT hr = S_OK;
  hr = web_browser2_->Navigate2(url.AsInput(), &empty, &empty, &empty, &empty);
  EXPECT_TRUE(hr == S_OK);
  return hr;
}

HRESULT IEEventSink::CloseWebBrowser() {
  if (!web_browser2_)
    return E_FAIL;

  DisconnectFromChromeFrame();
  EXPECT_HRESULT_SUCCEEDED(web_browser2_->Quit());
  return S_OK;
}

void IEEventSink::Refresh() {
  ScopedVariant refresh_level(REFRESH_NORMAL);
  web_browser2_->Refresh2(refresh_level.AsInput());
}

void IEEventSink::WaitForDOMAccessibilityTree() {
  if (IsCFRendering())
    WaitForChromeDOMAccessibilityTree(GetRendererWindow());
}

// private methods
void IEEventSink::ConnectToChromeFrame() {
  DCHECK(web_browser2_);
  if (chrome_frame_.get())
    return;
  ScopedComPtr<IShellBrowser> shell_browser;
  DoQueryService(SID_STopLevelBrowser, web_browser2_,
                 shell_browser.Receive());

  if (shell_browser) {
    ScopedComPtr<IShellView> shell_view;
    shell_browser->QueryActiveShellView(shell_view.Receive());
    if (shell_view) {
      shell_view->GetItemObject(SVGIO_BACKGROUND, __uuidof(IChromeFrame),
           reinterpret_cast<void**>(chrome_frame_.Receive()));
    }

    if (chrome_frame_) {
      ScopedVariant onmessage(onmessage_.ToDispatch());
      ScopedVariant onloaderror(onloaderror_.ToDispatch());
      ScopedVariant onload(onload_.ToDispatch());
      EXPECT_HRESULT_SUCCEEDED(chrome_frame_->put_onmessage(onmessage));
      EXPECT_HRESULT_SUCCEEDED(chrome_frame_->put_onloaderror(onloaderror));
      EXPECT_HRESULT_SUCCEEDED(chrome_frame_->put_onload(onload));
    }
  }
}

void IEEventSink::DisconnectFromChromeFrame() {
  if (chrome_frame_) {
    // Use a local ref counted copy of the IChromeFrame interface as the
    // outgoing calls could cause the interface to be deleted due to a message
    // pump running in the context of the outgoing call.
    ScopedComPtr<IChromeFrame> chrome_frame(chrome_frame_);
    chrome_frame_.Release();
    ScopedVariant dummy(static_cast<IDispatch*>(NULL));
    chrome_frame->put_onmessage(dummy);
    chrome_frame->put_onload(dummy);
    chrome_frame->put_onloaderror(dummy);
  }
}

void IEEventSink::FindIEProcessId() {
  HWND hwnd = NULL;
  web_browser2_->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&hwnd));
  EXPECT_TRUE(::IsWindow(hwnd));
  if (::IsWindow(hwnd))
    ::GetWindowThreadProcessId(hwnd, &ie_process_id_);
}

// Event callbacks
STDMETHODIMP_(void) IEEventSink::OnDownloadBegin() {
  if (listener_)
    listener_->OnDownloadBegin();
}

STDMETHODIMP_(void) IEEventSink::OnNewWindow2(IDispatch** disp,
                                              VARIANT_BOOL* s) {
  if (listener_)
    listener_->OnNewWindow2(disp, s);
}

STDMETHODIMP_(void) IEEventSink::OnNavigateError(IDispatch* dispatch,
    VARIANT* url, VARIANT* frame_name, VARIANT* status_code, VARIANT* cancel) {
  DLOG(INFO) << __FUNCTION__;
  if (listener_)
    listener_->OnNavigateError(dispatch, url, frame_name, status_code, cancel);
}

STDMETHODIMP IEEventSink::OnBeforeNavigate2(
    IDispatch* dispatch, VARIANT* url, VARIANT* flags,
    VARIANT* target_frame_name, VARIANT* post_data, VARIANT* headers,
    VARIANT_BOOL* cancel) {
  DLOG(INFO) << __FUNCTION__
      << StringPrintf("%ls - 0x%08X", url->bstrVal, this);
  // Reset any existing reference to chrome frame since this is a new
  // navigation.
  DisconnectFromChromeFrame();
  if (listener_)
    listener_->OnBeforeNavigate2(dispatch, url, flags, target_frame_name,
                                 post_data, headers, cancel);
  return S_OK;
}

STDMETHODIMP_(void) IEEventSink::OnNavigateComplete2(
    IDispatch* dispatch, VARIANT* url) {
  DLOG(INFO) << __FUNCTION__;
  ConnectToChromeFrame();
  if (listener_)
    listener_->OnNavigateComplete2(dispatch, url);
}

STDMETHODIMP_(void) IEEventSink::OnDocumentComplete(
    IDispatch* dispatch, VARIANT* url) {
  DLOG(INFO) << __FUNCTION__;
  EXPECT_TRUE(url);
  if (!url)
    return;
  if (listener_)
    listener_->OnDocumentComplete(dispatch, url);
}

STDMETHODIMP_(void) IEEventSink::OnFileDownload(
    VARIANT_BOOL active_doc, VARIANT_BOOL* cancel) {
  DLOG(INFO) << __FUNCTION__ << StringPrintf(" 0x%08X ad=%i", this,
                                              active_doc);
  if (listener_)
    listener_->OnFileDownload(active_doc, cancel);
  // Always cancel file downloads in tests.
  *cancel = VARIANT_TRUE;
}

STDMETHODIMP_(void) IEEventSink::OnNewWindow3(
    IDispatch** dispatch, VARIANT_BOOL* cancel, DWORD flags, BSTR url_context,
    BSTR url) {
  DLOG(INFO) << __FUNCTION__;
  EXPECT_TRUE(dispatch);
  if (!dispatch)
    return;

  if (listener_)
    listener_->OnNewWindow3(dispatch, cancel, flags, url_context, url);

  // Note that |dispatch| is an [in/out] argument. IE is asking listeners if
  // they want to use a IWebBrowser2 of their choice for the new window.
  // Since we need to listen on events on the new browser, we create one
  // if needed.
  if (!*dispatch) {
    ScopedComPtr<IDispatch> new_browser;
    HRESULT hr = new_browser.CreateInstance(CLSID_InternetExplorer, NULL,
                                            CLSCTX_LOCAL_SERVER);
    DCHECK(SUCCEEDED(hr) && new_browser);
    *dispatch = new_browser.Detach();
  }

  if (*dispatch)
    listener_->OnNewBrowserWindow(*dispatch, url);
}

STDMETHODIMP_(void) IEEventSink::OnQuit() {
  DLOG(INFO) << __FUNCTION__;

  did_receive_on_quit_ = true;

  DispEventUnadvise(web_browser2_);
  CoDisconnectObject(this, 0);

  if (listener_)
    listener_->OnQuit();
}

HRESULT IEEventSink::OnLoad(const VARIANT* param) {
  DLOG(INFO) << __FUNCTION__ << " " << param->bstrVal;
  ScopedVariant stack_object(*param);
  if (chrome_frame_) {
    if (listener_)
      listener_->OnLoad(param->bstrVal);
  } else {
    DLOG(WARNING) << "Invalid chrome frame pointer";
  }
  return S_OK;
}

HRESULT IEEventSink::OnLoadError(const VARIANT* param) {
  DLOG(INFO) << __FUNCTION__ << " " << param->bstrVal;
  if (chrome_frame_) {
    if (listener_)
      listener_->OnLoadError(param->bstrVal);
  } else {
    DLOG(WARNING) << "Invalid chrome frame pointer";
  }
  return S_OK;
}

HRESULT IEEventSink::OnMessage(const VARIANT* param) {
  DLOG(INFO) << __FUNCTION__ << " " << param;
  if (!chrome_frame_.get()) {
    DLOG(WARNING) << "Invalid chrome frame pointer";
    return S_OK;
  }

  ScopedVariant data, origin, source;
  if (param && (V_VT(param) == VT_DISPATCH)) {
    wchar_t* properties[] = { L"data", L"origin", L"source" };
    const int prop_count = arraysize(properties);
    DISPID ids[prop_count] = {0};

    HRESULT hr = param->pdispVal->GetIDsOfNames(IID_NULL, properties,
        prop_count, LOCALE_SYSTEM_DEFAULT, ids);
    if (SUCCEEDED(hr)) {
      DISPPARAMS params = { 0 };
      EXPECT_HRESULT_SUCCEEDED(param->pdispVal->Invoke(ids[0], IID_NULL,
          LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &params,
          data.Receive(), NULL, NULL));
      EXPECT_HRESULT_SUCCEEDED(param->pdispVal->Invoke(ids[1], IID_NULL,
          LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &params,
          origin.Receive(), NULL, NULL));
      EXPECT_HRESULT_SUCCEEDED(param->pdispVal->Invoke(ids[2], IID_NULL,
          LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &params,
          source.Receive(), NULL, NULL));
    }
  }

  if (listener_)
    listener_->OnMessage(V_BSTR(&data), V_BSTR(&origin), V_BSTR(&source));
  return S_OK;
}

}  // namespace chrome_frame_test

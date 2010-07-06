/*  Berkelium - Embedded Chromium
 *  Berkelium.cpp
 *
 *  Copyright (c) 2009, Patrick Reiter Horn
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Sirikata nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "chrome/browser/tab_contents/tab_contents.h"
#include "berkelium/Berkelium.hpp"
#include "Root.hpp"

////////////// Chrome Main function /////////////
// From chrome/app/chrome_dll_main.cc
#include "build/build_config.h"
#if defined(OS_WIN)
#include <algorithm>
#include <atlbase.h>
#include <atlapp.h>
#include <malloc.h>
#include <new.h>
#elif defined(OS_POSIX)
#include <locale.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(OS_LINUX)
#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>
#endif
#include "app/app_paths.h"
#include "app/resource_bundle.h"
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/debug_util.h"
#if defined(OS_POSIX)
#include "base/global_descriptors_posix.h"
#endif
#include "base/i18n/icu_util.h"
#include "base/message_loop.h"
#include "base/path_service.h"
#include "base/process_util.h"
#include "base/scoped_nsautorelease_pool.h"
#include "base/stats_counters.h"
#include "base/stats_table.h"
#include "base/string_util.h"
#if defined(OS_WIN)
#include "base/win_util.h"
#include "sandbox/src/sandbox_factory.h"
#include "sandbox/src/dep.h"
#endif
#if defined(OS_MACOSX)
#include "base/mac_util.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/app/breakpad_mac.h"
#endif
#if defined(OS_LINUX)
#include "base/nss_util.h"
#endif
#if defined(USE_LINUX_BREAKPAD)
#include "chrome/app/breakpad_linux.h"
#endif
#include "chrome/app/scoped_ole_initializer.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/common/url_constants.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_counters.h"
#include "chrome/common/chrome_descriptors.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/logging_chrome.h"
#include "chrome/common/main_function_params.h"
#include "chrome/common/sandbox_init_wrapper.h"
#include "ipc/ipc_switches.h"
#if defined(OS_WIN)
#include "sandbox/src/sandbox.h"
#include "tools/memory_watcher/memory_watcher.h"
#endif
#if defined(OS_MACOSX)
#include "third_party/WebKit/WebKit/mac/WebCoreSupport/WebSystemInterface.h"
#endif
#if defined(OS_LINUX)
#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include "chrome/browser/renderer_host/render_sandbox_host_linux.h"
#include "chrome/browser/zygote_host_linux.h"
#endif
//////////////////////

#if defined(OS_WIN)
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)

const wchar_t kProfilingDll[] = L"memory_watcher.dll";

// Load the memory profiling DLL.  All it needs to be activated
// is to be loaded.  Return true on success, false otherwise.
bool LoadMemoryProfiler() {
  HMODULE prof_module = LoadLibrary(kProfilingDll);
  return prof_module != NULL;
}

CAppModule _Module;

#pragma optimize("", off)
// Handlers for invalid parameter and pure call. They generate a breakpoint to
// tell breakpad that it needs to dump the process.
void InvalidParameter(const wchar_t* expression, const wchar_t* function,
                      const wchar_t* file, unsigned int line,
                      uintptr_t reserved) {
  if (Berkelium::Root::getSingleton().getErrorHandler())
    Berkelium::Root::getSingleton().getErrorHandler()->onInvalidParameter(expression, function, file, line);

  __debugbreak();
}

void PureCall() {
  if (Berkelium::Root::getSingleton().getErrorHandler())
    Berkelium::Root::getSingleton().getErrorHandler()->onPureCall();

  __debugbreak();
}

void OnNoMemory() {
  // Kill the process. This is important for security, since WebKit doesn't
  // NULL-check many memory allocations. If a malloc fails, returns NULL, and
  // the buffer is then used, it provides a handy mapping of memory starting at
  // address 0 for an attacker to utilize.
  if (Berkelium::Root::getSingleton().getErrorHandler())
    Berkelium::Root::getSingleton().getErrorHandler()->onOutOfMemory();

  __debugbreak();
}

// Handlers to silently dump the current process when there is an assert in
// chrome.
void ChromeAssert(const std::string& str) {
  // Get the breakpad pointer from chrome.exe
  if (Berkelium::Root::getSingleton().getErrorHandler())
    Berkelium::Root::getSingleton().getErrorHandler()->onAssertion(str.c_str());

  typedef void (__cdecl *DumpProcessFunction)();
  DumpProcessFunction DumpProcess = reinterpret_cast<DumpProcessFunction>(
      ::GetProcAddress(::GetModuleHandle(chrome::kBrowserProcessExecutableName),
                       "DumpProcess"));
  if (DumpProcess)
    DumpProcess();
}

#pragma optimize("", on)

// Early versions of Chrome incorrectly registered a chromehtml: URL handler,
// which gives us nothing but trouble. Avoid launching chrome this way since
// some apps fail to properly escape arguments.
bool HasDeprecatedArguments(const std::wstring& command_line) {
  const wchar_t kChromeHtml[] = L"chromehtml:";
  std::wstring command_line_lower = command_line;
  // We are only searching for ASCII characters so this is OK.
  StringToLowerASCII(&command_line_lower);
  std::wstring::size_type pos = command_line_lower.find(kChromeHtml);
  return (pos != std::wstring::npos);
}

//extern "C" int _set_new_mode(int);
#endif
extern int BrowserMain(const MainFunctionParams&);
extern int RendererMain(const MainFunctionParams&);
extern int GpuMain(const MainFunctionParams&);
extern int PluginMain(const MainFunctionParams&);
extern int WorkerMain(const MainFunctionParams&);
extern int NaClMain(const MainFunctionParams&);
extern int UtilityMain(const MainFunctionParams&);
extern int ProfileImportMain(const MainFunctionParams&);
extern int ZygoteMain(const MainFunctionParams&);
#if defined(_WIN64)
extern int NaClBrokerMain(const MainFunctionParams&);
#endif
extern int ServiceProcessMain(const MainFunctionParams&);

bool IsCrashReporterEnabled() {
    return false;
}

#ifdef OS_MACOSX
struct NSString;
void ClearCrashKeyValue(NSString*) {
}
void SetCrashKeyValue(NSString*, NSString*) {
}
void InitCrashReporter() {
}
void DestructCrashReporter() {
}
namespace browser_sync {
class TalkMediatorImpl {
public:
TalkMediatorImpl();
};
TalkMediatorImpl::TalkMediatorImpl() {
NOTREACHED();
}
}
#endif

namespace Berkelium {
namespace {
void CommonSubprocessInit() {
  // Initialize ResourceBundle which handles files loaded from external
  // sources.  The language should have been passed in to us from the
  // browser process as a command line flag.
  ResourceBundle::InitSharedInstance(std::wstring());

#if defined(OS_WIN)
  // HACK: Let Windows know that we have started.  This is needed to suppress
  // the IDC_APPSTARTING cursor from being displayed for a prolonged period
  // while a subprocess is starting.
  PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);
  MSG msg;
  PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
#endif
}

// Register the invalid param handler and pure call handler to be able to
// notify breakpad when it happens.
void RegisterInvalidParamHandler() {
#if defined(OS_WIN)
  _set_invalid_parameter_handler(InvalidParameter);
  _set_purecall_handler(PureCall);
  // Gather allocation failure.
  std::set_new_handler(&OnNoMemory);
  // Also enable the new handler for malloc() based failures.
  _set_new_mode(1);
#endif
}
void SetupCRT(const CommandLine& parsed_command_line) {
#if defined(OS_WIN)
#ifdef _CRTDBG_MAP_ALLOC
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
#else
  if (!parsed_command_line.HasSwitch(switches::kDisableBreakpad)) {
    _CrtSetReportMode(_CRT_ASSERT, 0);
  }
#endif

  // Enable the low fragmentation heap for the CRT heap. The heap is not changed
  // if the process is run under the debugger is enabled or if certain gflags
  // are set.
  bool use_lfh = false;
  if (parsed_command_line.HasSwitch(switches::kUseLowFragHeapCrt))
    use_lfh = parsed_command_line.GetSwitchValue(switches::kUseLowFragHeapCrt)
        != L"false";
  if (use_lfh) {
    void* crt_heap = reinterpret_cast<void*>(_get_heap_handle());
    ULONG enable_lfh = 2;
    HeapSetInformation(crt_heap, HeapCompatibilityInformation,
                       &enable_lfh, sizeof(enable_lfh));
  }
#endif
}
}

#if defined(OS_POSIX) && !defined(OS_MACOSX)
static void AdjustLinuxOOMScore(const std::string& process_type) {
  const int kMiscScore = 7;
  const int kPluginScore = 10;
  int score = -1;

  if (process_type == switches::kPluginProcess) {
    score = kPluginScore;
  } else if (process_type == switches::kUtilityProcess ||
             process_type == switches::kWorkerProcess ||
             process_type == switches::kGpuProcess ||
             process_type == switches::kServiceProcess) {
    score = kMiscScore;
  } else if (process_type == switches::kProfileImportProcess) {
    NOTIMPLEMENTED();
#ifndef DISABLE_NACL
  } else if (process_type == switches::kNaClLoaderProcess) {
    score = kPluginScore;
#endif
  } else if (process_type == switches::kZygoteProcess ||
             process_type.empty()) {
    // Pass - browser / zygote process stays at 0.
  } else if (process_type == switches::kExtensionProcess ||
             process_type == switches::kRendererProcess) {
    LOG(WARNING) << "process type '" << process_type << "' "
                 << "should go through the zygote.";
    // When debugging, these process types can end up being run directly.
    return;
  } else {
    NOTREACHED() << "Unknown process type";
  }
  if (score > -1)
    base::AdjustOOMScore(base::GetCurrentProcId(), score);
}
#endif  // defined(OS_POSIX) && !defined(OS_MACOSX)

#ifdef _WIN32
void forkedProcessHook(
    sandbox::BrokerServices* (*ptrGetBrokerServices)(),
    sandbox::TargetServices* (*ptrGetTargetServices)(),
    bool (*ptrSetCurrentProcessDEP)(enum sandbox::DepEnforcement))
{
#else
void forkedProcessHook(int argc, char **argv)
{
#endif
#if defined(OS_MACOSX)
  // TODO(mark): Some of these things ought to be handled in chrome_exe_main.mm.
  // Under the current architecture, nothing in chrome_exe_main can rely
  // directly on chrome_dll code on the Mac, though, so until some of this code
  // is refactored to avoid such a dependency, it lives here.  See also the
  // TODO(mark) below at InitCrashReporter() and DestructCrashReporter().
  base::EnableTerminationOnHeapCorruption();
#endif  // OS_MACOSX

  RegisterInvalidParamHandler();

  // The exit manager is in charge of calling the dtors of singleton objects.
  base::AtExitManager exit_manager;

  // We need this pool for all the objects created before we get to the
  // event loop, but we don't want to leave them hanging around until the
  // app quits. Each "main" needs to flush this pool right before it goes into
  // its main event loop to get rid of the cruft.
  base::ScopedNSAutoreleasePool autorelease_pool;

#if defined(OS_POSIX)
  base::GlobalDescriptors* g_fds = Singleton<base::GlobalDescriptors>::get();
  g_fds->Set(kPrimaryIPCChannel,
             kPrimaryIPCChannel + base::GlobalDescriptors::kBaseDescriptor);
#if defined(OS_LINUX)
  g_fds->Set(kCrashDumpSignal,
             kCrashDumpSignal + base::GlobalDescriptors::kBaseDescriptor);
#endif
#endif

#if defined(OS_POSIX)
  // Set C library locale to make sure CommandLine can parse argument values
  // in correct encoding.
  setlocale(LC_ALL, "");
#endif

  // Initialize the command line.
#if defined(OS_WIN)
  CommandLine::Init(0, NULL);
#else
  CommandLine::Init(argc, argv);
#endif
#if defined(OS_LINUX)
  // Set up CommandLine::SetProcTitle() support.
  // apparently no longer needed? CommandLine::SetTrueArgv(argv);
#endif

  const CommandLine& parsed_command_line = *CommandLine::ForCurrentProcess();
  std::wstring wide_process_type =
      parsed_command_line.GetSwitchValue(switches::kProcessType);
  std::string process_type(wide_process_type.begin(),wide_process_type.end());
#if defined(OS_MACOSX)
  mac_util::SetOverrideAppBundlePath(chrome::GetFrameworkBundlePath());
#endif  // OS_MACOSX

#if defined(OS_WIN)
  // Must do this before any other usage of command line!
  if (HasDeprecatedArguments(parsed_command_line.command_line_string()))
    return;
#endif

#if defined(OS_POSIX)
  // Always ignore SIGPIPE.  We check the return value of write().
  CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
#endif  // OS_POSIX

  int browser_pid;
  if (process_type.empty()) {
    browser_pid = base::GetCurrentProcId();
  } else {
#if defined(OS_WIN)
    std::wstring channel_name =
      parsed_command_line.GetSwitchValue(switches::kProcessChannelID);

    browser_pid = StringToInt(WideToASCII(channel_name));
    DCHECK(browser_pid != 0);
#else
    browser_pid = base::GetCurrentProcId();
#endif

#if defined(OS_POSIX)
    // When you hit Ctrl-C in a terminal running the browser
    // process, a SIGINT is delivered to the entire process group.
    // When debugging the browser process via gdb, gdb catches the
    // SIGINT for the browser process (and dumps you back to the gdb
    // console) but doesn't for the child processes, killing them.
    // The fix is to have child processes ignore SIGINT; they'll die
    // on their own when the browser process goes away.
    // Note that we *can't* rely on DebugUtil::BeingDebugged to catch this
    // case because we are the child process, which is not being debugged.
    if (!DebugUtil::BeingDebugged())
      signal(SIGINT, SIG_IGN);
#endif
  }
  SetupCRT(parsed_command_line);

  // Initialize the Chrome path provider.
  app::RegisterPathProvider();
  chrome::RegisterPathProvider();

  // Checks if the sandbox is enabled in this process and initializes it if this
  // is the case. The crash handler depends on this so it has to be done before
  // its initialization.
  SandboxInitWrapper sandbox_wrapper;
#if defined(OS_WIN)
  win_util::WinVersion win_version = win_util::GetWinVersion();
  if (win_version < win_util::WINVERSION_VISTA) {
    // On Vista, this is unnecessary since it is controlled through the
    // /NXCOMPAT linker flag.
    // Enforces strong DEP support.
    (*ptrSetCurrentProcessDEP)(sandbox::DEP_ENABLED);
  }

  // Get the interface pointer to the BrokerServices or TargetServices,
  // depending who we are.
  sandbox::SandboxInterfaceInfo sandbox_info = {0};
  sandbox_info.broker_services = (*ptrGetBrokerServices)();
  if (!sandbox_info.broker_services)
    sandbox_info.target_services = (*ptrGetTargetServices)();
  sandbox_wrapper.SetServices(&sandbox_info);
#endif
  sandbox_wrapper.InitializeSandbox(parsed_command_line, process_type);

#if defined(OS_WIN)
  _Module.Init(NULL, HINST_THISCOMPONENT);
#endif

#if defined(OS_MACOSX)
  // TODO(port-mac): This is from renderer_main_platform_delegate.cc.
  // shess tried to refactor things appropriately, but it sprawled out
  // of control because different platforms needed different styles of
  // initialization.  Try again once we understand the process
  // architecture needed and where it should live.
/*  if (single_process)
    InitWebCoreSystemInterface();
*/
#endif

  bool icu_result = icu_util::Initialize();
  CHECK(icu_result);

  logging::OldFileDeletionState file_state =
      logging::APPEND_TO_OLD_LOG_FILE;
  logging::InitChromeLogging(parsed_command_line, file_state);

#ifdef NDEBUG
  if (parsed_command_line.HasSwitch(switches::kSilentDumpOnDCHECK) &&
      parsed_command_line.HasSwitch(switches::kEnableDCHECK)) {
#if defined(OS_WIN)
    logging::SetLogReportHandler(ChromeAssert);
#endif
  }
#endif  // NDEBUG

  chrome::RegisterChromeSchemes(); // Required for "chrome-extension://" in InitExtensions

  if (!process_type.empty())
    CommonSubprocessInit();

  MainFunctionParams main_params(parsed_command_line, sandbox_wrapper,
                                 &autorelease_pool);

#if defined(OS_LINUX)
  AdjustLinuxOOMScore(process_type);
#endif

  // TODO(port): turn on these main() functions as they've been de-winified.
  int rv = -1;
  if (process_type == switches::kRendererProcess) {
    rv = RendererMain(main_params);
  } else if (process_type == switches::kExtensionProcess) {
    // An extension process is just a renderer process. We use a different
    // command line argument to differentiate crash reports.
    rv = RendererMain(main_params);
  } else if (process_type == switches::kPluginProcess) {
    rv = PluginMain(main_params);
  } else if (process_type == switches::kUtilityProcess) {
    rv = UtilityMain(main_params);
  } else if (process_type == switches::kGpuProcess) {
    rv = GpuMain(main_params);
  } else if (process_type == switches::kProfileImportProcess) {
#if defined(OS_MACOSX)
    rv = ProfileImportMain(main_params);
#else
    // TODO(port): Use OOP profile import - http://crbug.com/22142 .
    NOTIMPLEMENTED();
    rv = -1;
#endif
  } else if (process_type == switches::kWorkerProcess) {
    rv = WorkerMain(main_params);
#ifndef DISABLE_NACL
  } else if (process_type == switches::kNaClLoaderProcess) {
    rv = NaClMain(main_params);
#endif
#ifdef _WIN64  // The broker process is used only on Win64.
  } else if (process_type == switches::kNaClBrokerProcess) {
    rv = NaClBrokerMain(main_params);
#endif
  } else if (process_type == switches::kZygoteProcess) {
#if defined(OS_POSIX) && !defined(OS_MACOSX)
    // This function call can return multiple times, once per fork().
    if (ZygoteMain(main_params)) {
      // Zygote::HandleForkRequest may have reallocated the command
      // line so update it here with the new version.
      const CommandLine& parsed_command_line =
        *CommandLine::ForCurrentProcess();
      MainFunctionParams main_params(parsed_command_line, sandbox_wrapper,
                                     &autorelease_pool);
      std::string process_type =
        parsed_command_line.GetSwitchValueASCII(switches::kProcessType);
      if (process_type == switches::kRendererProcess ||
          process_type == switches::kExtensionProcess) {
        rv = RendererMain(main_params);
#ifndef DISABLE_NACL
      } else if (process_type == switches::kNaClLoaderProcess) {
        rv = NaClMain(main_params);
#endif
      } else {
        NOTREACHED() << "Unknown process type";
      }
    } else {
      rv = 0;
    }
#else
    NOTIMPLEMENTED();
#endif
  } else if (process_type == switches::kServiceProcess) {
    rv = ServiceProcessMain(main_params);
  } else if (process_type.empty()) {
#if defined(OS_LINUX)
    const char* sandbox_binary = NULL;
    struct stat st;

    // In Chromium branded builds, developers can set an environment variable to
    // use the development sandbox. See
    // http://code.google.com/p/chromium/wiki/LinuxSUIDSandboxDevelopment
    if (stat("/proc/self/exe", &st) == 0 && st.st_uid == getuid())
      sandbox_binary = getenv("CHROME_DEVEL_SANDBOX");

#if defined(LINUX_SANDBOX_PATH)
    if (!sandbox_binary)
      sandbox_binary = LINUX_SANDBOX_PATH;
#endif

    std::string sandbox_cmd;
    if (sandbox_binary && !parsed_command_line.HasSwitch(switches::kNoSandbox))
      sandbox_cmd = sandbox_binary;

    // Tickle the sandbox host and zygote host so they fork now.
    RenderSandboxHostLinux* shost = Singleton<RenderSandboxHostLinux>::get();
    shost->Init(sandbox_cmd);
    ZygoteHost* zhost = Singleton<ZygoteHost>::get();
    zhost->Init(sandbox_cmd);

    // We want to be sure to init NSPR on the main thread.
    base::EnsureNSPRInit();

    g_thread_init(NULL);
    // Glib type system initialization. Needed at least for gconf,
    // used in net/proxy/proxy_config_service_linux.cc. Most likely
    // this is superfluous as gtk_init() ought to do this. It's
    // definitely harmless, so retained as a reminder of this
    // requirement for gconf.
    g_type_init();
    // gtk_init() can change |argc| and |argv|.
    gtk_init(&argc, &argv);
    Root::SetUpGLibLogHandler();
#endif  // defined(OS_LINUX)

    rv = BrowserMain(main_params);
  } else {
    NOTREACHED() << "Unknown process type";
  }

#if defined(OS_WIN)
#ifdef _CRTDBG_MAP_ALLOC
  _CrtDumpMemoryLeaks();
#endif  // _CRTDBG_MAP_ALLOC

  _Module.Term();
#endif

  logging::CleanupChromeLogging();
}

void init() {
    new Root();
}
void destroy() {
    Root::destroy();
}
void update() {
    Root::getSingleton().update();
}
void setErrorHandler(ErrorDelegate *errorHandler) {
    Root::getSingleton().setErrorHandler(errorHandler);
}

}

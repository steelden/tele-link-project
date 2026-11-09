/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/launcher_win.h"

#include "core/crash_reports.h"
#include "core/update_checker.h"

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <VersionHelpers.h>

#include <crtdbg.h>
#include <cstdlib>

namespace Platform {
namespace {

// Turn a CRT/STL assertion (or pure-virtual / invalid-parameter) into a real,
// crash-reported failure instead of a modal Abort/Retry/Ignore dialog that
// would block a headless test agent forever.
[[noreturn]] void TestAgentReportCrash(
		const char *kind,
		const QString &message) {
	if (!message.isEmpty()) {
		CrashReports::SetAnnotation("CrtAssert", message);
	}
	// Be loud on stderr too (captured when the launch redirects it to a file).
	fprintf(stderr,
		"\n[testagent] %s: %s\n",
		kind,
		message.toLocal8Bit().constData());
	fflush(stderr);

	// Crashes with an access violation -> caught by the crash reporter, dumped
	// to tdata/working + tdata/dumps, and the process exits non-zero. No dialog.
	Unexpected("Test agent: CRT/STL assertion violation.");
}

#ifdef _DEBUG
int TestAgentReportHook(int reportType, char *message, int *returnValue) {
	if (returnValue) {
		*returnValue = 0; // Do not break into the (absent) debugger.
	}
	if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
		TestAgentReportCrash(
			"assert",
			QString::fromLocal8Bit(message ? message : ""));
	} else if (message) {
		fputs(message, stderr);
		fflush(stderr);
	}
	return TRUE; // Handled -> suppress the default message box.
}

int TestAgentReportHookW(int reportType, wchar_t *message, int *returnValue) {
	if (returnValue) {
		*returnValue = 0;
	}
	if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
		TestAgentReportCrash(
			"assert",
			QString::fromWCharArray(message ? message : L""));
	} else if (message) {
		fputws(message, stderr);
		fflush(stderr);
	}
	return TRUE;
}
#endif // _DEBUG

void InstallTestAgentCrashHandling() {
	// No Windows Error Reporting / general-protection / abort() message boxes —
	// each would block the test agent waiting on a button it cannot press.
	SetErrorMode(SEM_FAILCRITICALERRORS
		| SEM_NOGPFAULTERRORBOX
		| SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

	// Pure-virtual call / invalid CRT parameter -> reported crash, not a dialog.
	_set_purecall_handler([] {
		TestAgentReportCrash("purecall", u"Pure virtual function call"_q);
	});
	_set_invalid_parameter_handler([](
			const wchar_t *expression,
			const wchar_t *,
			const wchar_t *,
			unsigned int,
			uintptr_t) {
		TestAgentReportCrash(
			"invalid-parameter",
			QString::fromWCharArray(
				expression ? expression : L"CRT invalid parameter"));
	});

#ifdef _DEBUG
	// assert()/_ASSERTE and STL bounds/iterator checks (_STL_VERIFY) report via
	// _CrtDbgReport[W]; route them to stderr (never a window) and convert any
	// assertion into a reported crash through the hooks above.
	for (const auto type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT }) {
		_CrtSetReportMode(type, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
	}
	_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, TestAgentReportHook);
	_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, TestAgentReportHookW);
#endif // _DEBUG
}

// TeleLink: the crash reports are disabled in the build, a crash closed
// the app silently. A minidump is written to "crashes" near the exe, without
// a debugger (dbghelp is loaded only when the crash happens).
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS *info) {
	auto path = std::wstring(MAX_PATH, L'\0');
	const auto length = GetModuleFileNameW(
		nullptr,
		path.data(),
		DWORD(path.size()));
	path.resize(length);
	path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"crashes";
	CreateDirectoryW(path.c_str(), nullptr);

	auto now = SYSTEMTIME();
	GetLocalTime(&now);
	wchar_t name[64] = { 0 };
	swprintf_s(
		name,
		L"\\TeleLink_%04d-%02d-%02d_%02d-%02d-%02d.dmp",
		now.wYear,
		now.wMonth,
		now.wDay,
		now.wHour,
		now.wMinute,
		now.wSecond);
	path += name;

	using WriteDump = BOOL(WINAPI*)(
		HANDLE,
		DWORD,
		HANDLE,
		MINIDUMP_TYPE,
		PMINIDUMP_EXCEPTION_INFORMATION,
		PMINIDUMP_USER_STREAM_INFORMATION,
		PMINIDUMP_CALLBACK_INFORMATION);
	const auto dbghelp = LoadLibraryW(L"dbghelp.dll");
	const auto write = dbghelp
		? reinterpret_cast<WriteDump>(
			GetProcAddress(dbghelp, "MiniDumpWriteDump"))
		: nullptr;
	const auto file = write
		? CreateFileW(
			path.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr)
		: INVALID_HANDLE_VALUE;
	if (file != INVALID_HANDLE_VALUE) {
		auto exception = MINIDUMP_EXCEPTION_INFORMATION{
			.ThreadId = GetCurrentThreadId(),
			.ExceptionPointers = info,
			.ClientPointers = FALSE,
		};
		write(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			file,
			MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory
				| MiniDumpWithDataSegs
				| MiniDumpWithThreadInfo),
			&exception,
			nullptr,
			nullptr);
		CloseHandle(file);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

Launcher::Launcher(int argc, char *argv[])
: Core::Launcher(argc, argv) {
}

void Launcher::initHook() {
	SetUnhandledExceptionFilter(WriteCrashDump);
	if (cTestAgent()) {
		InstallTestAgentCrashHandling();
	}
}

std::optional<QStringList> Launcher::readArgumentsHook(
		int argc,
		char *argv[]) const {
	auto count = 0;
	if (const auto list = CommandLineToArgvW(GetCommandLine(), &count)) {
		const auto guard = gsl::finally([&] { LocalFree(list); });
		if (count > 0) {
			auto result = QStringList();
			result.reserve(count);
			for (auto i = 0; i != count; ++i) {
				result.push_back(QString::fromWCharArray(list[i]));
			}
			return result;
		}
	}
	return std::nullopt;
}

bool Launcher::launchUpdater(UpdaterLaunch action) {
	if (cExeName().isEmpty()) {
		return false;
	}

	const auto operation = (action == UpdaterLaunch::JustRelaunch)
		? QString()
		: (cWriteProtected()
			? u"runas"_q
			: QString());
	const auto binaryPath = (action == UpdaterLaunch::JustRelaunch)
		? (cExeDir() + cExeName())
		: (cWriteProtected()
			? (cWorkingDir() + u"tupdates/temp/Updater.exe"_q)
			: (cExeDir() + u"Updater.exe"_q));

	auto argumentsList = QStringList();
	const auto pushArgument = [&](const QString &argument) {
		argumentsList.push_back(argument.trimmed());
	};
	if (cLaunchMode() == LaunchModeAutoStart) {
		pushArgument(u"-autostart"_q);
	}
	if (Logs::DebugEnabled()) {
		pushArgument(u"-debug"_q);
	}
	if (cStartInTray()) {
		pushArgument(u"-startintray"_q);
	}
	if (customWorkingDir()) {
		pushArgument(u"-workdir"_q);
		pushArgument('"' + cWorkingDir() + '"');
	}
	if (cDataFile() != u"data"_q) {
		pushArgument(u"-key"_q);
		pushArgument('"' + cDataFile() + '"');
	}

	if (action == UpdaterLaunch::JustRelaunch) {
		pushArgument(u"-noupdate"_q);
		if (cRestartingToSettings()) {
			pushArgument(u"-tosettings"_q);
		}
	} else {
		pushArgument(u"-update"_q);
		auto created = FILETIME();
		auto exited = FILETIME();
		auto kernel = FILETIME();
		auto user = FILETIME();
		if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
			// WHY: Application and storage destruction precede this call, so
			// the updater can terminate us if remaining process teardown hangs.
			const auto creationTime = (quint64(created.dwHighDateTime) << 32)
				| created.dwLowDateTime;
			pushArgument(u"-finishprocess"_q);
			pushArgument(QString::number(GetCurrentProcessId()));
			pushArgument(QString::number(creationTime));
		} else {
			const auto error = GetLastError();
			LOG(("Update Error: could not query process creation time: %1"
				).arg(error));
		}
		pushArgument(u"-exename"_q);
		pushArgument('"' + cExeName() + '"');
		if (cWriteProtected()) {
			pushArgument(u"-writeprotected"_q);
			pushArgument('"' + cExeDir() + '"');
		}
	}
	return launch(operation, binaryPath, argumentsList);
}

bool Launcher::launch(
		const QString &operation,
		const QString &binaryPath,
		const QStringList &argumentsList) {
	const auto convertPath = [](const QString &path) {
		return QDir::toNativeSeparators(path).toStdWString();
	};
	const auto nativeBinaryPath = convertPath(binaryPath);
	const auto nativeWorkingDir = convertPath(cWorkingDir());
	const auto arguments = argumentsList.join(' ');

	Logs::closeMain();
	CrashReports::Finish();

	const auto hwnd = HWND(0);
	const auto result = ShellExecute(
		hwnd,
		operation.isEmpty() ? nullptr : operation.toStdWString().c_str(),
		nativeBinaryPath.c_str(),
		arguments.toStdWString().c_str(),
		nativeWorkingDir.empty() ? nullptr : nativeWorkingDir.c_str(),
		SW_SHOWNORMAL);
	return (int64(result) >= 32);
}

} // namespace Platform

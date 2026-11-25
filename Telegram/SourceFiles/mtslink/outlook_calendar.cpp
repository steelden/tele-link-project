/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/outlook_calendar.h"

#include "mtslink/call_window.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <atomic>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>
#endif // Q_OS_WIN

namespace MtsLink {
namespace {

// -1: not checked yet, 0: not available, 1: available.
std::atomic<int> Available = -1;

constexpr auto kEnabledKey = "outlookMeetingsEnabled";

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

// A recurring meeting without an end gives the occurrences till the end of
// the restriction: a limit anyway.
constexpr auto kMaxMeetings = 1000;
constexpr auto kFolderCalendar = 9; // olFolderCalendar.
constexpr auto kMeetingCanceled = 5; // olMeetingCanceled.
constexpr auto kMeetingReceivedAndCanceled = 7;

// Outlook is asked from one thread at a time.
std::mutex OutlookMutex;

class Variant final {
public:
	Variant() {
		VariantInit(&value);
	}
	explicit Variant(const wchar_t *text) : Variant() {
		value.vt = VT_BSTR;
		value.bstrVal = SysAllocString(text);
	}
	explicit Variant(const QString &text) : Variant() {
		value.vt = VT_BSTR;
		value.bstrVal = SysAllocStringLen(
			reinterpret_cast<const OLECHAR*>(text.utf16()),
			UINT(text.size()));
	}
	explicit Variant(long number) : Variant() {
		value.vt = VT_I4;
		value.lVal = number;
	}
	explicit Variant(bool flag) : Variant() {
		value.vt = VT_BOOL;
		value.boolVal = flag ? VARIANT_TRUE : VARIANT_FALSE;
	}
	~Variant() {
		VariantClear(&value);
	}
	Variant(const Variant &other) = delete;
	Variant &operator=(const Variant &other) = delete;

	VARIANT value;

};

bool Invoke(
		IDispatch *object,
		const wchar_t *name,
		WORD flags,
		VARIANT *args,
		int count,
		VARIANT *result) {
	if (!object) {
		return false;
	}
	auto id = DISPID();
	auto names = const_cast<LPOLESTR>(name);
	if (FAILED(object->GetIDsOfNames(
			IID_NULL,
			&names,
			1,
			LOCALE_USER_DEFAULT,
			&id))) {
		return false;
	}
	auto putId = DISPID(DISPID_PROPERTYPUT);
	auto params = DISPPARAMS{ args, nullptr, UINT(count), 0 };
	if (flags & DISPATCH_PROPERTYPUT) {
		params.rgdispidNamedArgs = &putId;
		params.cNamedArgs = 1;
	}
	return SUCCEEDED(object->Invoke(
		id,
		IID_NULL,
		LOCALE_USER_DEFAULT,
		flags,
		&params,
		result,
		nullptr,
		nullptr));
}

[[nodiscard]] ComPtr<IDispatch> GetDispatch(
		IDispatch *object,
		const wchar_t *name,
		WORD flags = DISPATCH_PROPERTYGET | DISPATCH_METHOD,
		VARIANT *args = nullptr,
		int count = 0) {
	auto result = Variant();
	if (!Invoke(object, name, flags, args, count, &result.value)
		|| result.value.vt != VT_DISPATCH
		|| !result.value.pdispVal) {
		return nullptr;
	}
	// The variant releases its reference, the pointer keeps its own.
	return ComPtr<IDispatch>(result.value.pdispVal);
}

[[nodiscard]] QString GetString(IDispatch *object, const wchar_t *name) {
	auto result = Variant();
	if (!Invoke(object, name, DISPATCH_PROPERTYGET, nullptr, 0, &result.value)
		|| FAILED(VariantChangeType(&result.value, &result.value, 0, VT_BSTR))
		|| !result.value.bstrVal) {
		return QString();
	}
	return QString::fromWCharArray(
		result.value.bstrVal,
		int(SysStringLen(result.value.bstrVal)));
}

[[nodiscard]] long GetNumber(IDispatch *object, const wchar_t *name) {
	auto result = Variant();
	if (!Invoke(object, name, DISPATCH_PROPERTYGET, nullptr, 0, &result.value)
		|| FAILED(VariantChangeType(&result.value, &result.value, 0, VT_I4))) {
		return 0;
	}
	return result.value.lVal;
}

// The local time of the calendar as a VT_DATE.
void SetDate(Variant &variant, const QDateTime &value) {
	const auto date = value.date();
	const auto time = value.time();
	auto system = SYSTEMTIME();
	system.wYear = WORD(date.year());
	system.wMonth = WORD(date.month());
	system.wDay = WORD(date.day());
	system.wHour = WORD(time.hour());
	system.wMinute = WORD(time.minute());
	system.wSecond = WORD(time.second());
	auto result = DATE();
	if (SystemTimeToVariantTime(&system, &result)) {
		variant.value.vt = VT_DATE;
		variant.value.date = result;
	}
}

[[nodiscard]] QDateTime GetDate(IDispatch *object, const wchar_t *name) {
	auto result = Variant();
	if (!Invoke(object, name, DISPATCH_PROPERTYGET, nullptr, 0, &result.value)
		|| result.value.vt != VT_DATE) {
		return QDateTime();
	}
	auto time = SYSTEMTIME();
	if (!VariantTimeToSystemTime(result.value.date, &time)) {
		return QDateTime();
	}
	// The local time of the calendar.
	return QDateTime(
		QDate(time.wYear, time.wMonth, time.wDay),
		QTime(time.wHour, time.wMinute, time.wSecond));
}

// The dates of the restriction are in the format of the user locale (as
// Outlook parses them: the short date and the time).
[[nodiscard]] QString FilterDate(const QDateTime &value) {
	const auto date = value.date();
	const auto time = value.time();
	auto system = SYSTEMTIME();
	system.wYear = WORD(date.year());
	system.wMonth = WORD(date.month());
	system.wDay = WORD(date.day());
	system.wHour = WORD(time.hour());
	system.wMinute = WORD(time.minute());
	wchar_t dateText[128] = { 0 };
	wchar_t timeText[128] = { 0 };
	if (!GetDateFormatEx(
			LOCALE_NAME_USER_DEFAULT,
			DATE_SHORTDATE,
			&system,
			nullptr,
			dateText,
			128,
			nullptr)
		|| !GetTimeFormatEx(
			LOCALE_NAME_USER_DEFAULT,
			TIME_NOSECONDS,
			&system,
			nullptr,
			timeText,
			128)) {
		return QString();
	}
	return QString::fromWCharArray(dateText)
		+ QChar(' ')
		+ QString::fromWCharArray(timeText);
}

[[nodiscard]] bool HasOutlookClsid(CLSID *clsid) {
	auto result = CLSID();
	if (FAILED(CLSIDFromProgID(L"Outlook.Application", &result))) {
		return false;
	}
	if (clsid) {
		*clsid = result;
	}
	return true;
}

// The MAPI namespace of the running Outlook, else started (it is one
// instance anyway).
[[nodiscard]] ComPtr<IDispatch> GetNamespace() {
	auto clsid = CLSID();
	if (!HasOutlookClsid(&clsid)) {
		return nullptr;
	}
	auto app = ComPtr<IDispatch>();
	auto running = ComPtr<IUnknown>();
	if (SUCCEEDED(GetActiveObject(clsid, nullptr, &running)) && running) {
		running.As(&app);
	}
	if (!app
		&& FAILED(CoCreateInstance(
			clsid,
			nullptr,
			CLSCTX_LOCAL_SERVER,
			IID_IDispatch,
			reinterpret_cast<void**>(app.GetAddressOf())))) {
		return nullptr;
	}
	auto mapi = Variant(L"MAPI");
	return GetDispatch(
		app.Get(),
		L"GetNamespace",
		DISPATCH_METHOD,
		&mapi.value,
		1);
}

[[nodiscard]] std::optional<std::vector<OutlookMeeting>> LoadMeetings(
		const QDateTime &from,
		const QDateTime &till) {
	const auto space = GetNamespace();
	if (!space) {
		return std::nullopt;
	}
	auto folderType = Variant(long(kFolderCalendar));
	const auto folder = GetDispatch(
		space.Get(),
		L"GetDefaultFolder",
		DISPATCH_METHOD,
		&folderType.value,
		1);
	const auto items = GetDispatch(folder.Get(), L"Items");
	if (!items) {
		return std::nullopt;
	}
	// The occurrences of the recurring meetings: sorted by the start first.
	auto sortBy = Variant(L"[Start]");
	Invoke(items.Get(), L"Sort", DISPATCH_METHOD, &sortBy.value, 1, nullptr);
	auto recurrences = Variant(true);
	Invoke(
		items.Get(),
		L"IncludeRecurrences",
		DISPATCH_PROPERTYPUT,
		&recurrences.value,
		1,
		nullptr);
	const auto fromText = FilterDate(from);
	const auto tillText = FilterDate(till);
	if (fromText.isEmpty() || tillText.isEmpty()) {
		return std::nullopt;
	}
	auto filter = Variant(u"[Start] < '%1' AND [End] > '%2'"_q.arg(
		tillText,
		fromText));
	const auto restricted = GetDispatch(
		items.Get(),
		L"Restrict",
		DISPATCH_METHOD,
		&filter.value,
		1);
	if (!restricted) {
		return std::nullopt;
	}
	auto result = std::vector<OutlookMeeting>();
	auto item = GetDispatch(restricted.Get(), L"GetFirst", DISPATCH_METHOD);
	for (auto i = 0; item && i != kMaxMeetings; ++i) {
		const auto status = GetNumber(item.Get(), L"MeetingStatus");
		auto meeting = OutlookMeeting{
			.subject = GetString(item.Get(), L"Subject").trimmed(),
			.location = GetString(item.Get(), L"Location").trimmed(),
			.start = GetDate(item.Get(), L"Start"),
			.end = GetDate(item.Get(), L"End"),
			.entryId = GetString(item.Get(), L"EntryID"),
			.recurring = (GetNumber(item.Get(), L"IsRecurring") != 0),
			.allDay = (GetNumber(item.Get(), L"AllDayEvent") != 0),
		};
		const auto canceled = (status == kMeetingCanceled)
			|| (status == kMeetingReceivedAndCanceled);
		if (!canceled
			&& meeting.start.isValid()
			&& meeting.start < till
			&& (!meeting.end.isValid() || meeting.end > from)) {
			result.push_back(std::move(meeting));
		}
		item = GetDispatch(restricted.Get(), L"GetNext", DISPATCH_METHOD);
	}
	ranges::sort(result, ranges::less(), &OutlookMeeting::start);
	return result;
}

// The inspector of the item: of the occurrence (by its start) for a
// recurring one, of the series if not found (a moved occurrence).
[[nodiscard]] QString DisplayMeeting(const OutlookMeeting &meeting) {
	const auto space = GetNamespace();
	if (!space) {
		return u"no namespace"_q;
	}
	auto id = Variant(meeting.entryId);
	const auto item = GetDispatch(
		space.Get(),
		L"GetItemFromID",
		DISPATCH_METHOD,
		&id.value,
		1);
	if (!item) {
		return u"no item"_q;
	}
	auto target = item;
	auto kind = u"item"_q;
	if (meeting.recurring) {
		const auto pattern = GetDispatch(
			item.Get(),
			L"GetRecurrencePattern",
			DISPATCH_METHOD);
		auto start = Variant();
		SetDate(start, meeting.start);
		if (const auto occurrence = pattern
			? GetDispatch(
				pattern.Get(),
				L"GetOccurrence",
				DISPATCH_METHOD,
				&start.value,
				1)
			: nullptr) {
			target = occurrence;
			kind = u"occurrence"_q;
		} else {
			kind = u"series"_q;
		}
	}
	auto modal = Variant(false);
	return Invoke(
		target.Get(),
		L"Display",
		DISPATCH_METHOD,
		&modal.value,
		1,
		nullptr)
		? (kind + u" shown"_q)
		: (kind + u" not shown"_q);
}

// COM in the worker thread: initialized for the call, all the objects are
// released before it is uninitialized.
template <typename Callback>
auto WithCom(Callback &&callback) -> decltype(callback()) {
	const auto initialized = CoInitializeEx(
		nullptr,
		COINIT_APARTMENTTHREADED);
	auto result = callback();
	if (SUCCEEDED(initialized)) {
		CoUninitialize();
	}
	return result;
}

#endif // Q_OS_WIN

} // namespace

bool outlookSupported() {
#ifdef Q_OS_WIN
	return true;
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

void checkOutlookAvailable(Fn<void()> checked) {
#ifdef Q_OS_WIN
	crl::async([=] {
		const auto available = WithCom([] {
			return HasOutlookClsid(nullptr);
		});
		Available = available ? 1 : 0;
		crl::on_main([=] {
			LOG(("MtsLink Outlook: %1"
				).arg(available ? "available" : "not available"));
			if (checked) {
				checked();
			}
		});
	});
#else // Q_OS_WIN
	Available = 0;
	if (checked) {
		checked();
	}
#endif // Q_OS_WIN
}

bool outlookAvailable() {
	return (Available.load() == 1);
}

bool outlookMeetingsEnabled() {
	return outlookSupported()
		&& readLocalSettings().value(
			QString::fromLatin1(kEnabledKey)).toBool();
}

void setOutlookMeetingsEnabled(bool enabled) {
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kEnabledKey), enabled);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the meetings %1"
		).arg(enabled ? "enabled" : "disabled"));
}

void testOutlookConnection(Fn<void(bool)> done) {
#ifdef Q_OS_WIN
	const auto from = QDateTime(QDate::currentDate(), QTime(0, 0));
	const auto till = from.addDays(1);
	crl::async([=] {
		auto ok = false;
		try {
			const auto lock = std::lock_guard<std::mutex>(OutlookMutex);
			ok = WithCom([&] {
				return LoadMeetings(from, till).has_value();
			});
		} catch (...) {
			ok = false;
		}
		if (ok) {
			Available = 1;
		}
		crl::on_main([=] {
			LOG(("MtsLink Outlook: the test connection %1"
				).arg(ok ? "works" : "failed"));
			done(ok);
		});
	});
#else // Q_OS_WIN
	done(false);
#endif // Q_OS_WIN
}

void loadOutlookMeetings(
		QDateTime from,
		QDateTime till,
		Fn<void(std::optional<std::vector<OutlookMeeting>>)> done) {
#ifdef Q_OS_WIN
	if (Available.load() == 0) {
		done(std::nullopt);
		return;
	}
	crl::async([=] {
		auto result = std::optional<std::vector<OutlookMeeting>>();
		try {
			const auto lock = std::lock_guard<std::mutex>(OutlookMutex);
			result = WithCom([&] {
				return LoadMeetings(from, till);
			});
		} catch (...) {
			result = std::nullopt;
		}
		crl::on_main([=, result = std::move(result)]() mutable {
			done(std::move(result));
		});
	});
#else // Q_OS_WIN
	done(std::nullopt);
#endif // Q_OS_WIN
}

void openOutlookMeeting(const OutlookMeeting &meeting) {
#ifdef Q_OS_WIN
	if (meeting.entryId.isEmpty()) {
		LOG(("MtsLink Outlook: open, no entry id"));
		return;
	}
	// The inspector of Outlook is brought to the front.
	AllowSetForegroundWindow(ASFW_ANY);
	crl::async([=] {
		auto result = QString();
		try {
			const auto lock = std::lock_guard<std::mutex>(OutlookMutex);
			result = WithCom([&] {
				return DisplayMeeting(meeting);
			});
		} catch (...) {
			result = u"exception"_q;
		}
		crl::on_main([=] {
			LOG(("MtsLink Outlook: open (recurring: %1): %2"
				).arg(meeting.recurring ? "yes" : "no", result));
		});
	});
#endif // Q_OS_WIN
}

} // namespace MtsLink

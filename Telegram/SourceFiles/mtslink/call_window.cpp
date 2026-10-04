/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/call_window.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "main/main_account.h"
#include "mtslink/rpc.h"
#include "mtslink/session.h"
#include "lang/lang_keys.h"
#include "lang/lang_instance.h"
#include "mtslink/data_adapters.h"
#include "mtslink/env_config.h"
#include "webview/webview_embed.h"
#include "window/main_window.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkCookie>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace MtsLink {
namespace {

constexpr auto kDefaultWidth = 1200;
constexpr auto kDefaultHeight = 800;

class CallWindow final : public QWidget {
public:
	CallWindow(const QString &url, const QString &title, bool video);

	[[nodiscard]] bool valid() const;
	[[nodiscard]] const QString &url() const;

protected:
	void closeEvent(QCloseEvent *e) override;

private:
	QString _url;
	QVBoxLayout *_layout = nullptr;
	std::unique_ptr<Webview::Window> _webview;

};

// The session of TeleLink in the browser: the conference opens for the
// same user, whatever was signed in the browser profile before.
[[nodiscard]] std::vector<Webview::Cookie> SessionCookies() {
	const auto host = QUrl(EnvConfig::instance().httpsServerUrl()).host();
	const auto parent = host.section('.', 1);
	const auto fallback = parent.isEmpty() ? host : (u"."_q + parent);
	auto result = std::vector<Webview::Cookie>();
	for (const auto &cookie : fileAuthCookies()) {
		auto domain = cookie.domain();
		if (domain.isEmpty()) {
			domain = fallback;
		} else if (!domain.startsWith('.')) {
			// "mts-link.ru" in the browser is only for that host, the
			// session is needed on gw.mts-link.ru and others as well.
			domain = u"."_q + domain;
		}
		LOG(("MtsLink Call: cookie %1 domain=%2 path=%3 expires=%4"
			).arg(QString::fromLatin1(cookie.name())
			).arg(domain
			).arg(cookie.path()
			).arg(cookie.expirationDate().toString(Qt::ISODate)));
		result.push_back({
			.name = cookie.name().toStdString(),
			.value = cookie.value().toStdString(),
			.domain = domain.toStdString(),
			.path = (cookie.path().isEmpty()
				? u"/"_q
				: cookie.path()).toStdString(),
			.httpOnly = cookie.isHttpOnly(),
			.secure = cookie.isSecure(),
		});
	}
	LOG(("MtsLink Call: %1 session cookies").arg(result.size()));
	return result;
}

// Runs before the page scripts: the MTS Link client takes its language from
// localStorage first ("i18nextLng").
[[nodiscard]] QByteArray InitScript(const QString &language, bool video) {
	return R"JS(
(function() {
	var lang = ")JS" + language.toUtf8() + R"JS(";
	var audioOnly = )JS" + QByteArray(video ? "false" : "true") + R"JS(;
	try {
		localStorage.setItem('i18nextLng', lang);
		localStorage.setItem('systemLocale', lang);
		// The conference room without a signed in user.
		localStorage.setItem('locale', lang.toUpperCase());
	} catch (e) {}
	var post = function(obj) {
		try { window.chrome.webview.postMessage(JSON.stringify(obj)); }
		catch (e) {}
	};
	// The conference room takes the language of a signed in user from the
	// profile ("locale" of /api/login), it is shown in the language of
	// TeleLink without changing the profile on the server.
	var isProfileUrl = function(url) {
		return /\/api\/(login|user|users\/\d+)(\?|$)/.test(String(url || ''));
	};
	var withLocale = function(text) {
		try {
			var data = JSON.parse(text);
			if (data && typeof data === 'object' && 'locale' in data
				&& data.locale !== lang.toUpperCase()) {
				post({ type: 'locale', was: data.locale, now: lang.toUpperCase() });
				data.locale = lang.toUpperCase();
				return JSON.stringify(data);
			}
		} catch (e) {}
		return text;
	};
	try {
		var fetchWithoutLocale = window.fetch;
		window.fetch = function(input, init) {
			var url = (typeof input === 'string') ? input : (input && input.url);
			var result = fetchWithoutLocale.apply(this, arguments);
			if (!isProfileUrl(url)) {
				return result;
			}
			return result.then(function(r) {
				if (!r.ok) {
					return r;
				}
				return r.clone().text().then(function(text) {
					return new Response(withLocale(text), {
						status: r.status,
						statusText: r.statusText,
						headers: r.headers,
					});
				}, function() { return r; });
			});
		};
		var xhrOpen = XMLHttpRequest.prototype.open;
		XMLHttpRequest.prototype.open = function(method, url) {
			if (isProfileUrl(url)) {
				var xhr = this;
				xhr.addEventListener('readystatechange', function() {
					if (xhr.readyState !== 4 || xhr.status !== 200) return;
					try {
						var text = withLocale(xhr.responseText);
						Object.defineProperty(xhr, 'responseText', { value: text });
						Object.defineProperty(xhr, 'response', {
							value: (xhr.responseType === 'json')
								? JSON.parse(text)
								: text,
						});
					} catch (e) {}
				});
			}
			return xhrOpen.apply(this, arguments);
		};
	} catch (e) {}
	// An audio call: the camera is not given to the page at the start, it
	// can be turned on later in the conference.
	if (audioOnly && navigator.mediaDevices
		&& navigator.mediaDevices.getUserMedia) {
		var started = Date.now();
		var getUserMedia = navigator.mediaDevices.getUserMedia.bind(
			navigator.mediaDevices);
		navigator.mediaDevices.getUserMedia = function(constraints) {
			if (constraints && constraints.video
				&& Date.now() - started < 15000) {
				post({ type: 'audio-only', note: 'camera request skipped' });
				var copy = Object.assign({}, constraints);
				delete copy.video;
				if (!copy.audio) {
					return Promise.reject(new DOMException(
						'Audio call', 'NotAllowedError'));
				}
				return getUserMedia(copy);
			}
			return getUserMedia(constraints);
		};
	}
	// Where the conference takes its language from.
	setTimeout(function() {
		var found = {};
		try {
			for (var i = 0; i < localStorage.length; ++i) {
				var key = localStorage.key(i);
				var value = localStorage.getItem(key) || '';
				if (/lang|locale|i18n/i.test(key) || /^"?(ru|en)/i.test(value)) {
					found[key] = value.slice(0, 80);
				}
			}
		} catch (e) {}
		var scripts = [];
		var nodes = document.querySelectorAll('script[src]');
		for (var j = 0; j < nodes.length && j < 15; ++j) {
			scripts.push(nodes[j].src);
		}
		post({
			type: 'language',
			htmlLang: document.documentElement.lang,
			navigator: navigator.language,
			cookie: document.cookie.split(';').filter(function(c) {
				return /lang|locale|i18n/i.test(c);
			}),
			storage: found,
			scripts: scripts,
		});
	}, 8000);
	// Whether the browser session is signed in for the conference.
	var checkLogin = function(when) {
		fetch('https://gw.mts-link.ru/api/login', { credentials: 'include' })
			.then(function(r) {
				return r.text().then(function(t) {
					post({ type: 'login', when: when, status: r.status, body: t.slice(0, 120) });
				});
			}).catch(function(e) {
				post({ type: 'login', when: when, error: String(e) });
			});
	};
	if (location.host.indexOf('my.') === 0) {
		checkLogin('start');
	}
	// Diagnostics: the page address changes (SPA navigation).
	var lastUrl = location.href;
	var urlTimer = setInterval(function() {
		if (location.href !== lastUrl) {
			lastUrl = location.href;
			post({ type: 'url', url: lastUrl });
		}
	}, 300);
	setTimeout(function() { clearInterval(urlTimer); }, 120000);
	var logged = false;
	var promptLogged = false;
	var findButton = function(text) {
		var nodes = document.querySelectorAll('button, a, [role="button"]');
		for (var i = 0; i < nodes.length; ++i) {
			if ((nodes[i].innerText || '').trim() === text) {
				return nodes[i];
			}
		}
		return null;
	};
	var clickLikeMouse = function(text) {
		var button = findButton(text);
		if (!button) {
			post({ type: 'auto-click', result: 'button is gone', url: location.href });
			return;
		}
		var rect = button.getBoundingClientRect();
		var x = rect.left + rect.width / 2;
		var y = rect.top + rect.height / 2;
		var target = document.elementFromPoint(x, y) || button;
		post({
			type: 'auto-click',
			url: location.href,
			target: target.tagName + '.' + String(target.className || '').slice(0, 60),
			inside: button.contains(target),
		});
		var init = {
			bubbles: true,
			cancelable: true,
			composed: true,
			view: window,
			clientX: x,
			clientY: y,
			button: 0,
			buttons: 1,
			pointerId: 1,
			pointerType: 'mouse',
			isPrimary: true,
		};
		try { target.dispatchEvent(new PointerEvent('pointerover', init)); } catch (e) {}
		try { target.dispatchEvent(new PointerEvent('pointerdown', init)); } catch (e) {}
		target.dispatchEvent(new MouseEvent('mousedown', init));
		init.buttons = 0;
		try { target.dispatchEvent(new PointerEvent('pointerup', init)); } catch (e) {}
		target.dispatchEvent(new MouseEvent('mouseup', init));
		target.dispatchEvent(new MouseEvent('click', init));
		setTimeout(function() {
			post({ type: 'after-click', url: location.href });
		}, 1500);
	};
	var check = function() {
		var nodes = document.querySelectorAll('button, a, [role="button"]');
		var texts = [];
		for (var i = 0; i < nodes.length; ++i) {
			var text = (nodes[i].innerText || '').trim();
			if (!text) continue;
			texts.push(text);
			var lower = text.toLowerCase();
			if ((lower.indexOf('браузер') >= 0
					|| lower.indexOf('browser') >= 0)
				&& lower.indexOf('прилож') < 0
				&& lower.indexOf('app') < 0) {
				// A plain element.click() goes to the guest join, the button
				// is clicked like a mouse does.
				if (!promptLogged) {
					promptLogged = true;
					post({
						type: 'browser-prompt',
						text: text,
						url: location.href,
						tag: nodes[i].tagName,
						html: (nodes[i].outerHTML || '').slice(0, 300),
					});
					setTimeout(function() { clickLikeMouse(text); }, 0);
				}
				return;
			}
		}
		if (!logged && texts.length) {
			logged = true;
			post({ type: 'buttons', texts: texts.slice(0, 30) });
		}
	};
	var start = function() {
		check();
		new MutationObserver(check).observe(
			document.documentElement,
			{ childList: true, subtree: true });
	};
	if (document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', start);
	} else {
		start();
	}
})();
)JS";
}

std::vector<CallWindow*> &Windows() {
	static auto result = std::vector<CallWindow*>();
	return result;
}

CallWindow::CallWindow(
	const QString &url,
	const QString &title,
	bool video)
: _url(url)
, _layout(new QVBoxLayout(this)) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(title.isEmpty()
		? tr::lng_mtslink_call_window(tr::now)
		: tr::lng_mtslink_call_window_chat(tr::now, lt_chat, title));
	setWindowIcon(Window::CreateIcon());
	_layout->setContentsMargins(0, 0, 0, 0);
	resize(kDefaultWidth, kDefaultHeight);

	// The same browser profile as the sign in: the conference opens for
	// the signed in user.
	const auto language = Lang::GetInstance().id().startsWith(u"ru"_q)
		? u"ru"_q
		: u"en"_q;
	_webview = std::make_unique<Webview::Window>(
		this,
		Webview::WindowConfig{
			.opaqueBg = QColor(255, 255, 255),
			.storageId = Webview::StorageId{
				.path = cWorkingDir() + u"tdata/mtslink_auth"_q,
				.token = Webview::LegacyStorageIdToken(),
			},
			.allowThirdPartyCookies = true,
			.allowMediaPermissions = true,
			.language = language,
			.cookies = SessionCookies(),
		});
	if (!_webview->valid()) {
		LOG(("MtsLink Call: embedded browser is not available"));
		return;
	}
	if (const auto widget = _webview->widget()) {
		_layout->addWidget(widget);
	}
	_webview->setMessageHandler([=](const QJsonDocument &message) {
		LOG(("MtsLink Call: page %1").arg(
			QString::fromUtf8(message.toJson(QJsonDocument::Compact))));
	});
	_webview->init(InitScript(language, video));
	_webview->setNavigationStartHandler([=](QString url, bool newWindow) {
		LOG(("MtsLink Call: navigate %1%2"
			).arg(url, newWindow ? u" (new window)"_q : QString()));
		// After leaving the conference the page goes to the meeting rating
		// and the meeting info, the call is finished.
		if (url.contains(u"/event-rating/"_q)
			|| url.contains(u"/event/"_q)) {
			LOG(("MtsLink Call: finished, closing the window"));
			crl::on_main(this, [=] { close(); });
			return false;
		}
		return true;
	});
	_webview->navigate(url);
	LOG(("MtsLink Call: opened %1 in the embedded browser").arg(url));
}

bool CallWindow::valid() const {
	return _webview && _webview->valid();
}

const QString &CallWindow::url() const {
	return _url;
}

void CallWindow::closeEvent(QCloseEvent *e) {
	auto &windows = Windows();
	windows.erase(ranges::remove(windows, this), end(windows));
	// Leaves the conference: the page is destroyed with the browser.
	_webview = nullptr;
	QWidget::closeEvent(e);
}

} // namespace

namespace {

[[nodiscard]] QString SettingsPath() {
	return cWorkingDir() + u"tdata/mtslink_settings.json"_q;
}

[[nodiscard]] QJsonObject ReadSettings() {
	auto file = QFile(SettingsPath());
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.readAll()).object()
		: QJsonObject();
}

} // namespace

QHash<QString, QString> CallEvents; // joinLink -> webinarEventId

void rememberCallEvent(const QString &joinLink, const QString &eventId) {
	if (!joinLink.isEmpty() && !eventId.isEmpty()) {
		CallEvents.insert(joinLink, eventId);
	}
}

void joinCallLink(
		const QString &joinLink,
		const QString &title,
		CallBrowser browser,
		bool video) {
	const auto eventId = CallEvents.value(joinLink);
	const auto mts = Core::App().activeAccount().mtsLinkSession();
	const auto rpc = mts ? mts->rpc() : nullptr;
	if (eventId.isEmpty() || !rpc) {
		LOG(("MtsLink Call: no event for %1, opening as is").arg(joinLink));
		openCallLink(joinLink, title, browser, video);
		return;
	}
	LOG(("MtsLink Call: GenerateWebinarLink event=%1").arg(eventId));
	rpc->call(
		u"WebinarApp.GenerateWebinarLink"_q,
		QJsonObject{
			{ u"webinarEventId"_q, eventId },
			{ u"platform"_q, u"Web"_q },
		},
		[=](const QJsonObject &result) {
			const auto link = result.value(u"value"_q).toObject().value(
				u"link"_q).toString();
			LOG(("MtsLink Call: personal link %1").arg(link));
			openCallLink(
				link.isEmpty() ? joinLink : link,
				title,
				browser,
				video);
		},
		[=](const QString &error) {
			LOG(("MtsLink Call: GenerateWebinarLink failed: %1").arg(error));
			openCallLink(joinLink, title, browser, video);
		});
}

QJsonObject readLocalSettings() {
	return ReadSettings();
}

void writeLocalSettings(const QJsonObject &settings) {
	auto file = QFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(settings).toJson());
	}
}

bool callsInEmbeddedBrowser() {
	return ReadSettings().value(u"callsInEmbeddedBrowser"_q).toBool(true);
}

void setCallsInEmbeddedBrowser(bool embedded) {
	auto settings = ReadSettings();
	settings.insert(u"callsInEmbeddedBrowser"_q, embedded);
	auto file = QFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(settings).toJson());
	}
	LOG(("MtsLink Call: embedded browser %1").arg(embedded ? 1 : 0));
}

void openCallLink(
		const QString &url,
		const QString &title,
		CallBrowser browser,
		bool video) {
	const auto system = (browser == CallBrowser::System)
		|| (browser == CallBrowser::Default && !callsInEmbeddedBrowser());
	if (url.isEmpty()) {
		return;
	} else if (system) {
		File::OpenUrl(url);
		return;
	}
	for (const auto window : Windows()) {
		if (window->url() == url) {
			window->showNormal();
			window->raise();
			window->activateWindow();
			return;
		}
	}
	const auto window = new CallWindow(url, title, video);
	if (!window->valid()) {
		delete window;
		File::OpenUrl(url);
		return;
	}
	Windows().push_back(window);
	window->show();
	window->raise();
	window->activateWindow();
}

} // namespace MtsLink

/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/auth_widget.h"

#include "webview/webview_embed.h"
#include "base/options.h"
#include "core/application.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

namespace MtsLink {
namespace {

const auto kSigninUrl =
	"https://my.mts-link.ru/signin";

} // namespace

AuthWidget::AuthWidget(QWidget *parent)
: QWidget(parent) {
	_layout = new QVBoxLayout(this);
	_layout->setContentsMargins(0, 0, 0, 0);

	_statusLabel = new QLabel(this);
	_statusLabel->setAlignment(Qt::AlignCenter);
	_statusLabel->hide();

	_retryButton = new QPushButton(
		QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd1\x8c"),
		this);
	_retryButton->hide();
	QObject::connect(
		_retryButton,
		&QPushButton::clicked,
		this,
		&AuthWidget::startAuth);

	QObject::connect(
		&_auth,
		&Api::Auth::authSuccess,
		this,
		&AuthWidget::authCompleted);
	QObject::connect(
		&_auth,
		&Api::Auth::authFailed,
		this,
		[this](const QString &error) {
			showError(error);
			Q_EMIT authFailed(error);
		});
}

AuthWidget::~AuthWidget() = default;

void AuthWidget::startAuth() {
	_statusLabel->hide();
	_retryButton->hide();

	createWebView();
	_webView->navigate(QString(kSigninUrl));
}

void AuthWidget::createWebView() {
	base::options::lookup<bool>(
		Webview::kOptionWebviewDebugEnabled).set(true);

	const auto storagePath = cWorkingDir()
		+ u"tdata/mtslink_auth"_q;
	_webView = std::make_unique<Webview::Window>(
		this,
		Webview::WindowConfig{
			.opaqueBg = QColor(255, 255, 255),
			.storageId = Webview::StorageId{
				.path = storagePath,
				.token = Webview::LegacyStorageIdToken(),
			},
			.allowThirdPartyCookies = true,
		});

	if (!_webView->valid()) {
		showError("WebView not available");
		return;
	}

	if (auto *widget = _webView->widget()) {
		_layout->insertWidget(0, widget);
	}

	_webView->setNavigationStartHandler([this](QString url, bool newWindow) {
		LOG(("MtsLink Auth: navigation start: %1 (new=%2)")
			.arg(url).arg(newWindow));
		return onNavigationStart(url, newWindow);
	});

	_webView->setNavigationDoneHandler([this](bool success) {
		LOG(("MtsLink Auth: navigation done, success=%1").arg(success));
		if (!success) {
			LOG(("MtsLink Auth: navigation FAILED"));
		}
		_webView->eval(R"JS(
			(function() {
				var info = {
					url: location.href,
					title: document.title,
					bodyText: document.body
						? document.body.innerText.substring(0, 500)
						: '(no body)',
				};
				window.chrome.webview.postMessage(
					JSON.stringify(info));
			})();
		)JS");
	});

	_webView->setMessageHandler([this](const QJsonDocument &message) {
		LOG(("MtsLink Auth WebView message: %1")
			.arg(QString::fromUtf8(message.toJson(
				QJsonDocument::Compact))));
	});
}

bool AuthWidget::onNavigationStart(const QString &url, bool newWindow) {
	if (newWindow) {
		return false;
	}

	const auto parsed = QUrl(url);
	const auto path = parsed.path();

	if (path.contains("transfer-organization")
		|| path.contains("sso-signin")) {
		const auto query = QUrlQuery(parsed);
		const auto authCode = query.queryItemValue("authCode");
		if (!authCode.isEmpty()) {
			onAuthCodeReceived(authCode);
			return false;
		}
	}
	return true;
}

void AuthWidget::onAuthCodeReceived(const QString &authCode) {
	if (auto *widget = _webView ? _webView->widget() : nullptr) {
		widget->hide();
	}
	showLoading(QString::fromUtf8(
		"\xd0\x90\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb7"
		"\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f..."));

	_auth.loginByAuthCode(authCode);
}

void AuthWidget::showError(const QString &text) {
	if (auto *widget = _webView ? _webView->widget() : nullptr) {
		widget->hide();
	}
	_statusLabel->setText(QString::fromUtf8(
		"\xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0: ") + text);
	_statusLabel->show();
	_retryButton->show();

	if (_layout->indexOf(_statusLabel) < 0) {
		_layout->addWidget(_statusLabel);
		_layout->addWidget(_retryButton);
	}
}

void AuthWidget::showLoading(const QString &text) {
	_statusLabel->setText(text);
	_statusLabel->show();
	_retryButton->hide();

	if (_layout->indexOf(_statusLabel) < 0) {
		_layout->addWidget(_statusLabel);
	}
}

} // namespace MtsLink

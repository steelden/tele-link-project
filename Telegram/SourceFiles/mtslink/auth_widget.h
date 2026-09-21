/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.

SSO WebView widget: opens MTS Link signin page,
intercepts authCode from redirect, exchanges for JWT.
*/
#pragma once

#include "mtslink/api/api_auth.h"

#include <QtWidgets/QWidget>

namespace Webview {
class Window;
} // namespace Webview

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace MtsLink {

class AuthWidget final : public QWidget {
	Q_OBJECT

public:
	explicit AuthWidget(QWidget *parent = nullptr);
	~AuthWidget();

	void startAuth();

Q_SIGNALS:
	void authCompleted(const Api::AuthResult &result);
	void authFailed(const QString &error);

private:
	void createWebView();
	bool onNavigationStart(const QString &url, bool newWindow);
	void onAuthCodeReceived(const QString &authCode);
	void showError(const QString &text);
	void showLoading(const QString &text);

	Api::Auth _auth;
	std::unique_ptr<Webview::Window> _webView;
	QVBoxLayout *_layout = nullptr;
	QLabel *_statusLabel = nullptr;
	QPushButton *_retryButton = nullptr;
};

} // namespace MtsLink

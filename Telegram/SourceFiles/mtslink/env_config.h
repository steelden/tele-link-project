/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include <QObject>
#include <QString>
#include <functional>

namespace MtsLink {

class EnvConfig final : public QObject {
	Q_OBJECT

public:
	static EnvConfig &instance();

	void load(const QString &envConfigUrl);
	void setCachePath(const QString &path);

	[[nodiscard]] QString wssServerUrl() const;
	[[nodiscard]] QString httpsServerUrl() const;
	[[nodiscard]] QString baseMediaUrl() const;
	[[nodiscard]] QString publicCdnMediaUrl() const;
	[[nodiscard]] QString webinarHost() const;

	[[nodiscard]] bool isLoaded() const;

Q_SIGNALS:
	void loaded();
	void loadFailed();

private:
	EnvConfig();

	void parse(const QByteArray &data);
	void saveToCache(const QByteArray &data);
	bool loadFromCache();

	QString _cachePath;
	QString _wssServerUrl;
	QString _httpsServerUrl;
	QString _baseMediaUrl;
	QString _publicCdnMediaUrl;
	QString _webinarHost;
	bool _loaded = false;
};

} // namespace MtsLink

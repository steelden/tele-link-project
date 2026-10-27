/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.

The language of a code block by its content.
*/
#include "mtslink/code_language.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

#include <array>

namespace MtsLink {

QString GuessCodeLanguage(const QString &code) {
	const auto trimmed = code.trimmed();
	if (trimmed.isEmpty()) {
		return QString();
	}
	if ((trimmed.startsWith('{') && trimmed.endsWith('}'))
		|| (trimmed.startsWith('[') && trimmed.endsWith(']'))) {
		const auto valid = [](const QString &json) {
			auto error = QJsonParseError();
			QJsonDocument::fromJson(json.toUtf8(), &error);
			return (error.error == QJsonParseError::NoError);
		};
		// The JSON of the settings (VS Code, Sublime) has the comments and
		// the trailing commas.
		static const auto kLineComment = QRegularExpression(
			u"^\\s*//[^\\n]*$"_q,
			QRegularExpression::MultilineOption);
		static const auto kTrailingComma = QRegularExpression(
			u",(\\s*[\\]}])"_q);
		if (valid(trimmed)
			|| valid(QString(trimmed)
				.replace(kLineComment, QString())
				.replace(kTrailingComma, u"\\1"_q))) {
			return u"json"_q;
		}
	}
	struct Rule {
		const char *language = nullptr;
		const char *pattern = nullptr;
		int weight = 0;
		bool caseInsensitive = false;
	};
	static const auto kRules = std::array<Rule, 74>{ {
		// SQL.
		{ "sql", R"(\bselect\b[\s\S]+?\bfrom\b)", 3, true },
		{ "sql", R"(\binsert\s+into\b)", 4, true },
		{ "sql", R"(\bupdate\s+\w[\w.]*\s+set\b)", 4, true },
		{ "sql", R"(\bdelete\s+from\b)", 4, true },
		{ "sql", R"(\b(create|alter|drop)\s+(or\s+replace\s+)?((unique|clustered|nonclustered|temp|temporary|global\s+temporary|materialized|unlogged|if\s+(not\s+)?exists)\s+)*(table|index|view|function|procedure|trigger|schema|sequence|database|type|extension)\b)", 5, true },
		{ "sql", R"(\b(inner|left|right|full)\s+(outer\s+)?join\b)", 3, true },
		{ "sql", R"(\b(where|group\s+by|order\s+by|having)\b)", 1, true },
		{ "sql", R"(\b(not\s+null|primary\s+key|foreign\s+key|varchar|bigint|timestamptz)\b)", 2, true },
		// C#.
		{ "csharp", R"(^\s*using\s+[A-Z][\w.]*\s*;)", 4 },
		{ "csharp", R"(^\s*namespace\s+[\w.]+\s*[;{]?\s*$)", 4 },
		{ "csharp", R"(\{\s*get\s*;\s*(private\s+|init\s*;|set\s*;))", 5 },
		{ "csharp", R"(\b(public|private|internal|protected)\s+(static\s+)?(async\s+)?(Task|void|string|int|bool|var)\b)", 2 },
		{ "csharp", R"(\basync\s+Task\b|\bawait\b)", 2 },
		{ "csharp", R"(\bvar\s+\w+\s*=)", 2 },
		{ "csharp", R"(\bConsole\.Write|\bstring\.\w+\(|\b\w+\?\.\w+)", 3 },
		{ "csharp", R"(^\s*\[[A-Z]\w*(\(.*\))?\]\s*$)", 2 },
		{ "csharp", R"(\bnameof\s*\()", 5 },
		{ "csharp", R"(\b(const|readonly)\s+(int|uint|long|ulong|short|byte|string|bool|double|float|decimal|char|object|TimeSpan|Guid|DateTime)\??\s+\w+)", 4 },
		{ "csharp", R"(\b(internal|sealed|partial|override|virtual)\s+(static\s+)?(class|record|struct|interface|void|async|Task|string|int|bool)\b)", 3 },
		{ "csharp", R"(\brecord\s+(struct\s+|class\s+)?[A-Z]\w*\s*[({])", 4 },
		{ "csharp", R"(\$@?"|\bis\s+(not\s+)?null\b|\btypeof\s*\(\s*[A-Z])", 3 },
		// The style: the braces on their own lines, the properties in
		// PascalCase (blockedBy.Value), the nullable values.
		{ "csharp", R"(^\s*\{\s*$)", 2 },
		{ "csharp", R"(\b[a-z_]\w*\.[A-Z]\w*\b)", 2 },
		{ "csharp", R"(\.(Value|HasValue)\b)", 2 },
		// Java.
		{ "java", R"(^\s*package\s+[\w.]+\s*;)", 5 },
		{ "java", R"(^\s*import\s+(static\s+)?java[x]?\.)", 5 },
		{ "java", R"(\bSystem\.out\.print)", 5 },
		{ "java", R"(@Override|@Autowired|@Service|@RestController)", 4 },
		{ "java", R"(\bpublic\s+static\s+void\s+main\s*\(\s*String)", 5 },
		{ "java", R"(\b(private|public)\s+final\b|\bString\[\])", 2 },
		// Kotlin.
		{ "kotlin", R"(^\s*fun\s+\w+\s*\()", 5 },
		{ "kotlin", R"(^\s*(val|var)\s+\w+\s*(:\s*\w+)?\s*=)", 2 },
		{ "kotlin", R"(\bdata\s+class\b|^\s*import\s+kotlinx?\.)", 5 },
		// JavaScript.
		{ "javascript", R"(\bfunction\s*\w*\s*\()", 2 },
		{ "javascript", R"(^\s*(const|let)\s+\w+\s*=)", 3 },
		{ "javascript", R"(\bconsole\.(log|error|warn)\s*\()", 5 },
		{ "javascript", R"(\brequire\s*\(\s*['"])", 5 },
		{ "javascript", R"(^\s*import\s+.*\s+from\s+['"])", 4 },
		{ "javascript", R"(^\s*export\s+(default\s+)?(function|const|class)\b)", 3 },
		{ "javascript", R"(\bdocument\.|\bwindow\.|===)", 2 },
		// TypeScript.
		{ "typescript", R"(^\s*(export\s+)?(interface|type)\s+\w+\s*(=|\{))", 5 },
		{ "typescript", R"(\w\s*:\s*(string|number|boolean|void|any)\b)", 3 },
		// Python.
		{ "python", R"(^\s*def\s+\w+\s*\(.*\)\s*(->\s*[\w\[\], .]+)?:\s*$)", 5 },
		{ "python", R"(^\s*from\s+[\w.]+\s+import\s+)", 5 },
		{ "python", R"(^\s*import\s+[\w.]+\s*$)", 2 },
		{ "python", R"(^\s*(if|elif|for|while|with|class)\b.*:\s*$)", 2 },
		{ "python", R"(\bself\.\w+|\bprint\s*\(|\bNone\b|\bTrue\b|\bFalse\b)", 1 },
		// Go.
		{ "go", R"(^\s*package\s+\w+\s*$)", 4 },
		{ "go", R"(^\s*func\s+(\(\w+\s+\*?\w+\)\s*)?\w+\s*\()", 5 },
		{ "go", R"(\w\s*:=|\bfmt\.)", 3 },
		// C++.
		{ "cpp", R"(^\s*#include\s*[<"])", 5 },
		{ "cpp", R"(\bstd::|\bnullptr\b|\btemplate\s*<)", 4 },
		{ "cpp", R"(\bauto\s+\w+\s*=|->\w+\()", 1 },
		// Bash.
		{ "bash", R"(^#!/(usr/)?bin/(env\s+)?(ba|z)?sh)", 8 },
		{ "bash", R"(^\s*(sudo|echo|export|cd|apt(-get)?|curl|chmod|mkdir|grep)\s)", 2 },
		{ "bash", R"(^\s*(fi|done|esac)\s*$|\$\{\w+\}|\|\s*grep\b)", 3 },
		// PowerShell.
		{ "powershell", R"(\b(Get|Set|New|Remove|Write|Invoke|Start)-[A-Z]\w+)", 4 },
		{ "powershell", R"(\$env:\w+|-ErrorAction\b)", 4 },
		// XML / HTML.
		{ "xml", R"(^\s*<\?xml\b)", 8 },
		{ "html", R"(<!DOCTYPE\s+html|<(html|head|body|div|span|script)\b)", 5, true },
		{ "xml", R"(^\s*<[\w:.-]+(\s+[\w:.-]+="[^"]*")*\s*/?>)", 2 },
		// CSS.
		{ "css", R"(^\s*[.#]?[\w-]+(\s*[,>+~]?\s*[.#]?[\w-]+)*\s*\{\s*$)", 1 },
		{ "css", R"(^\s*[\w-]+\s*:\s*[^;{}]+;\s*$)", 1 },
		{ "css", R"(\b(color|margin|padding|font-size|display|background)\s*:)", 3 },
		// YAML.
		{ "yaml", R"(^\s*[\w.-]+:\s*$)", 1 },
		{ "yaml", R"(^\s*-\s+[\w.-]+:\s)", 2 },
		{ "yaml", R"(^(apiVersion|kind|metadata|spec|services|version|openapi|swagger|info|paths|servers|components|jobs|stages|steps|volumes|networks|dependencies):)", 4 },
		{ "yaml", R"(^\s*[\w.-]+:\s+[^,;{}()\[\]]+$)", 1 },
		{ "yaml", R"(^\s*[\w.-]+:\s+(\[\]|\{\})\s*$)", 2 },
		{ "yaml", R"(^\s*[\w.-]+:\s+"[^"]*"\s*$)", 1 },
		// Dockerfile.
		{ "dockerfile", R"(^\s*FROM\s+[\w./:-]+(\s+AS\s+\w+)?\s*$)", 5 },
		{ "dockerfile", R"(^\s*(RUN|COPY|ENTRYPOINT|CMD|WORKDIR|EXPOSE)\s)", 3 },
		// PHP.
		{ "php", R"(<\?php|\$this->)", 8 },
	} };
	static const auto kCompiled = [] {
		auto result = std::vector<QRegularExpression>();
		result.reserve(kRules.size());
		for (const auto &rule : kRules) {
			auto options = QRegularExpression::PatternOptions(
				QRegularExpression::MultilineOption);
			if (rule.caseInsensitive) {
				options |= QRegularExpression::CaseInsensitiveOption;
			}
			result.emplace_back(QString::fromUtf8(rule.pattern), options);
		}
		return result;
	}();
	auto scores = QHash<QString, int>();
	for (auto i = 0; i != int(kRules.size()); ++i) {
		auto matches = kCompiled[i].globalMatch(trimmed);
		auto count = 0;
		while (matches.hasNext() && count < 3) {
			matches.next();
			++count;
		}
		if (count) {
			scores[QString::fromUtf8(kRules[i].language)]
				+= kRules[i].weight * count;
		}
	}
	// TypeScript is JavaScript with the types.
	if (scores.value(u"typescript"_q) > 0) {
		scores[u"typescript"_q] += scores.value(u"javascript"_q);
	}
	auto best = QString();
	auto bestScore = 0;
	auto secondScore = 0;
	for (auto i = scores.cbegin(); i != scores.cend(); ++i) {
		if (i.value() > bestScore) {
			secondScore = bestScore;
			bestScore = i.value();
			best = i.key();
		} else if (i.value() > secondScore) {
			secondScore = i.value();
		}
	}
	constexpr auto kMinScore = 4;
	return (bestScore >= kMinScore && bestScore > secondScore)
		? best
		: QString();
}

} // namespace MtsLink

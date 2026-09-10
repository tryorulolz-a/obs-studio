#ifdef _WIN32

#include "ReplayBufferAIEditor.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/bmem.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr auto kSettingsOrg = "ReplayBuffer";
constexpr auto kSettingsApp = "OBS-AI";
constexpr auto kOrigin = "obs-replaybuffer-ai";

struct ReadyItem {
	QString id;
	QString outputPath;
	QString exportPath;
	QString reason;
	QString style;
	QString semanticEventType;
	QString suggestedCaption;
	double rankScore = 0.0;
	double highlightScore = 0.0;
	double criticScore = 0.0;
	int fps = 0;
	bool captionsBurnedIn = false;
};

QSettings Settings()
{
	return QSettings(QSettings::IniFormat, QSettings::UserScope, kSettingsOrg, kSettingsApp);
}

QString AppDataReplayBuffer()
{
	QString root = qEnvironmentVariable("APPDATA");
	if (root.isEmpty())
		root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	return QDir(root).filePath(QStringLiteral("ReplayBuffer"));
}

QJsonObject ReadObject(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	QJsonParseError error{};
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return {};
	return document.object();
}

QString SocialAspect(const QString &preset)
{
	if (preset == QStringLiteral("instagram_feed_4x5"))
		return QStringLiteral("4:5");
	if (preset == QStringLiteral("square_1x1"))
		return QStringLiteral("1:1");
	if (preset.endsWith(QStringLiteral("16x9")))
		return QStringLiteral("16:9");
	return QStringLiteral("9:16");
}

QPair<int, int> SocialSize(const QString &preset)
{
	if (preset == QStringLiteral("instagram_feed_4x5"))
		return {1080, 1350};
	if (preset == QStringLiteral("square_1x1"))
		return {1080, 1080};
	if (preset.endsWith(QStringLiteral("16x9")))
		return {1920, 1080};
	return {1080, 1920};
}

bool IsOwnJob(const QJsonObject &job)
{
	return job.value(QStringLiteral("origin")).toString() == QString::fromLatin1(kOrigin);
}

QList<ReadyItem> ReadReadyItems(const QString &jobsRoot)
{
	QList<ReadyItem> result;
	QDir root(jobsRoot);
	const auto dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
	for (const QFileInfo &dirInfo : dirs) {
		const QString dir = dirInfo.absoluteFilePath();
		const QJsonObject job = ReadObject(QDir(dir).filePath(QStringLiteral("job.json")));
		if (!IsOwnJob(job))
			continue;
		const QJsonObject out = ReadObject(QDir(dir).filePath(QStringLiteral("result.json")));
		if (out.isEmpty() || !out.value(QStringLiteral("ok")).toBool(false))
			continue;
		const QString output = out.value(QStringLiteral("outputPath")).toString();
		if (output.isEmpty() || !QFileInfo::exists(output))
			continue;
		ReadyItem item;
		item.id = job.value(QStringLiteral("jobId")).toString(dirInfo.fileName());
		item.outputPath = output;
		item.exportPath = out.value(QStringLiteral("exportPath")).toString();
		item.reason = job.value(QStringLiteral("reason")).toString();
		item.style = out.value(QStringLiteral("style")).toString(job.value(QStringLiteral("style")).toString());
		item.semanticEventType = out.value(QStringLiteral("semanticEventType")).toString();
		item.suggestedCaption = out.value(QStringLiteral("suggestedCaption")).toString();
		item.rankScore = out.value(QStringLiteral("rankScore")).toDouble();
		item.highlightScore = out.value(QStringLiteral("highlightScore")).toDouble();
		item.criticScore = out.value(QStringLiteral("criticScore")).toDouble();
		item.fps = out.value(QStringLiteral("fps")).toInt();
		item.captionsBurnedIn = out.value(QStringLiteral("captionsBurnedIn")).toBool();
		result.push_back(std::move(item));
		if (result.size() >= 50)
			break;
	}
	std::sort(result.begin(), result.end(), [](const ReadyItem &a, const ReadyItem &b) {
		if (a.rankScore != b.rankScore)
			return a.rankScore > b.rankScore;
		return a.id > b.id;
	});
	return result;
}

QStringList PendingJobs(const QString &jobsRoot)
{
	QStringList result;
	QDir root(jobsRoot);
	const auto dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo &dirInfo : dirs) {
		const QString dir = dirInfo.absoluteFilePath();
		const QString jobPath = QDir(dir).filePath(QStringLiteral("job.json"));
		const QJsonObject job = ReadObject(jobPath);
		if (!IsOwnJob(job))
			continue;
		if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("result.json"))))
			result.push_back(jobPath);
	}
	return result;
}
} // namespace

ReplayBufferAIEditor::ReplayBufferAIEditor(QWidget *parent) : QWidget(parent)
{
	BuildUi();
	LoadSettings();

	workerProcess_ = new QProcess(this);
	workerProcess_->setProcessChannelMode(QProcess::MergedChannels);
	connect(workerProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		[this](int code, QProcess::ExitStatus status) {
			if (status == QProcess::CrashExit)
				status_->setText(tr("AI worker crashed. Check the job result/log and retry."));
			else if (code != 0)
				status_->setText(tr("AI worker exited with code %1.").arg(code));
			activeJobId_.clear();
			RefreshState();
			QTimer::singleShot(100, this, [this] { DispatchNext(); });
		});

	doctorProcess_ = new QProcess(this);
	doctorProcess_->setProcessChannelMode(QProcess::MergedChannels);
	connect(doctorProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		[this](int code, QProcess::ExitStatus status) {
			const QString detail = QString::fromUtf8(doctorProcess_->readAll()).trimmed();
			const bool ok = status == QProcess::NormalExit && code == 0;
			runtime_->setText(ok ? tr("Runtime: ready") : tr("Runtime: not ready — %1").arg(detail.isEmpty() ? tr("doctor failed") : detail));
		});

	obs_frontend_add_event_callback(&ReplayBufferAIEditor::FrontendEvent, this);

	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, [this] {
		RefreshState();
		DispatchNext();
	});
	timer_->start();
	RefreshState();
	DispatchNext();
}

ReplayBufferAIEditor::~ReplayBufferAIEditor()
{
	obs_frontend_remove_event_callback(&ReplayBufferAIEditor::FrontendEvent, this);
	if (workerProcess_ && workerProcess_->state() != QProcess::NotRunning) {
		const QString dir = QFileInfo(workerProcess_->property("jobPath").toString()).absolutePath();
		if (!dir.isEmpty()) {
			QFile cancel(QDir(dir).filePath(QStringLiteral("cancel.requested")));
			if (cancel.open(QIODevice::WriteOnly | QIODevice::Truncate))
				cancel.write("OBS frontend closing\n");
		}
		workerProcess_->waitForFinished(1500);
	}
}

void ReplayBufferAIEditor::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	auto *top = new QHBoxLayout;
	enabled_ = new QCheckBox(tr("Enable local AI editing"), this);
	autoReplay_ = new QCheckBox(tr("Auto-edit saved replays"), this);
	captions_ = new QCheckBox(tr("Captions"), this);
	top->addWidget(enabled_);
	top->addWidget(autoReplay_);
	top->addWidget(captions_);
	top->addStretch(1);
	root->addLayout(top);

	auto *form = new QFormLayout;
	form->setContentsMargins(0, 0, 0, 0);
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(4);

	style_ = new QComboBox(this);
	style_->addItem(tr("Viral gaming"), QStringLiteral("viral_gaming"));
	style_->addItem(tr("Clean competitive"), QStringLiteral("clean_competitive"));
	style_->addItem(tr("Fast vertical"), QStringLiteral("fast_vertical"));
	style_->addItem(tr("Cinematic"), QStringLiteral("cinematic"));
	style_->addItem(tr("Streamer / reaction"), QStringLiteral("streamer"));
	form->addRow(tr("Edit style"), style_);

	social_ = new QComboBox(this);
	social_->addItem(tr("TikTok / Reels 9:16"), QStringLiteral("tiktok_9x16"));
	social_->addItem(tr("YouTube Shorts 9:16"), QStringLiteral("youtube_shorts_9x16"));
	social_->addItem(tr("Instagram Feed 4:5"), QStringLiteral("instagram_feed_4x5"));
	social_->addItem(tr("Square 1:1"), QStringLiteral("square_1x1"));
	social_->addItem(tr("YouTube 16:9"), QStringLiteral("youtube_16x9"));
	form->addRow(tr("Export"), social_);

	reframe_ = new QComboBox(this);
	reframe_->addItem(tr("Smart crop"), QStringLiteral("smart_crop"));
	reframe_->addItem(tr("Auto"), QStringLiteral("auto"));
	reframe_->addItem(tr("Full frame"), QStringLiteral("full_frame"));
	reframe_->addItem(tr("FPS HUD stack"), QStringLiteral("hud_stack"));
	form->addRow(tr("Reframe"), reframe_);

	color_ = new QComboBox(this);
	color_->addItem(tr("Vibrant"), QStringLiteral("vibrant"));
	color_->addItem(tr("Balanced auto"), QStringLiteral("auto"));
	color_->addItem(tr("Original"), QStringLiteral("original"));
	color_->addItem(tr("Strong saturation"), QStringLiteral("strong"));
	form->addRow(tr("Color"), color_);

	smoothFps_ = new QComboBox(this);
	smoothFps_->addItem(tr("Native cadence"), QStringLiteral("native"));
	smoothFps_->addItem(tr("Interpolate to 60 FPS"), QStringLiteral("interpolate_60"));
	form->addRow(tr("Frame rate"), smoothFps_);

	prompt_ = new QLineEdit(this);
	prompt_->setPlaceholderText(tr("Optional edit direction, e.g. keep clutch + reaction, fast hook"));
	prompt_->setMaxLength(2000);
	form->addRow(tr("Prompt"), prompt_);

	worker_ = new QLineEdit(this);
	worker_->setPlaceholderText(tr("LocalEditorWorker.exe"));
	auto *workerRow = new QHBoxLayout;
	workerRow->setContentsMargins(0, 0, 0, 0);
	workerRow->addWidget(worker_, 1);
	auto *browseWorker = new QPushButton(tr("Browse"), this);
	workerRow->addWidget(browseWorker);
	auto *workerHost = new QWidget(this);
	workerHost->setLayout(workerRow);
	form->addRow(tr("Worker"), workerHost);
	root->addLayout(form);

	auto *actions = new QHBoxLayout;
	auto *apply = new QPushButton(tr("Apply"), this);
	auto *queueLast = new QPushButton(tr("Edit Last Replay"), this);
	auto *queueFile = new QPushButton(tr("Edit File…"), this);
	auto *montage = new QPushButton(tr("Best Montage"), this);
	auto *check = new QPushButton(tr("Check Runtime"), this);
	auto *setup = new QPushButton(tr("Setup Runtime"), this);
	auto *retry = new QPushButton(tr("Retry Failed"), this);
	actions->addWidget(apply);
	actions->addWidget(queueLast);
	actions->addWidget(queueFile);
	actions->addWidget(montage);
	actions->addStretch(1);
	actions->addWidget(check);
	actions->addWidget(setup);
	actions->addWidget(retry);
	root->addLayout(actions);

	status_ = new QLabel(tr("AI editor idle"), this);
	status_->setWordWrap(true);
	runtime_ = new QLabel(tr("Runtime: not checked"), this);
	runtime_->setWordWrap(true);
	root->addWidget(status_);
	root->addWidget(runtime_);

	auto *activeRow = new QHBoxLayout;
	active_ = new QLabel(tr("No active edit"), this);
	progress_ = new QProgressBar(this);
	progress_->setRange(0, 100);
	progress_->setValue(0);
	progress_->setTextVisible(true);
	cancel_ = new QPushButton(tr("Cancel"), this);
	cancel_->setEnabled(false);
	activeRow->addWidget(active_, 1);
	activeRow->addWidget(progress_, 2);
	activeRow->addWidget(cancel_);
	root->addLayout(activeRow);

	auto *readyGroup = new QGroupBox(tr("Ready AI edits"), this);
	auto *readyLayout = new QVBoxLayout(readyGroup);
	readyLayout->setContentsMargins(6, 6, 6, 6);
	readyLayout->setSpacing(4);
	ready_ = new QListWidget(readyGroup);
	ready_->setSelectionMode(QAbstractItemView::SingleSelection);
	readyLayout->addWidget(ready_, 1);
	auto *readyActions = new QHBoxLayout;
	auto *open = new QPushButton(tr("Open"), readyGroup);
	auto *copyCaption = new QPushButton(tr("Copy Caption"), readyGroup);
	auto *like = new QPushButton(tr("Like"), readyGroup);
	auto *dislike = new QPushButton(tr("Dislike"), readyGroup);
	auto *remove = new QPushButton(tr("Delete Edit"), readyGroup);
	readyActions->addWidget(open);
	readyActions->addWidget(copyCaption);
	readyActions->addStretch(1);
	readyActions->addWidget(like);
	readyActions->addWidget(dislike);
	readyActions->addWidget(remove);
	readyLayout->addLayout(readyActions);
	root->addWidget(readyGroup, 1);

	connect(apply, &QPushButton::clicked, this, [this] { SaveSettings(); });
	connect(queueLast, &QPushButton::clicked, this, [this] { QueueLastReplay(); });
	connect(queueFile, &QPushButton::clicked, this, [this] { QueueFile(); });
	connect(montage, &QPushButton::clicked, this, [this] { QueueBestMontage(); });
	connect(check, &QPushButton::clicked, this, [this] { CheckRuntime(); });
	connect(setup, &QPushButton::clicked, this, [this] { SetupRuntime(); });
	connect(retry, &QPushButton::clicked, this, [this] { RetryFailed(); });
	connect(cancel_, &QPushButton::clicked, this, [this] { CancelActive(); });
	connect(open, &QPushButton::clicked, this, [this] { OpenSelected(); });
	connect(like, &QPushButton::clicked, this, [this] { RateSelected(true); });
	connect(dislike, &QPushButton::clicked, this, [this] { RateSelected(false); });
	connect(remove, &QPushButton::clicked, this, [this] { DeleteSelected(); });
	connect(ready_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { OpenSelected(); });
	connect(copyCaption, &QPushButton::clicked, this, [this] {
		const QString id = SelectedReadyId();
		if (id.isEmpty())
			return;
		const auto items = ReadReadyItems(JobsRoot());
		for (const auto &item : items) {
			if (item.id == id && !item.suggestedCaption.isEmpty()) {
				QGuiApplication::clipboard()->setText(item.suggestedCaption);
				status_->setText(tr("Suggested caption copied."));
				break;
			}
		}
	});
	connect(browseWorker, &QPushButton::clicked, this, [this] {
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose LocalEditorWorker"), QFileInfo(WorkerPath()).absolutePath(), tr("Executables (*.exe)"));
		if (!path.isEmpty()) {
			worker_->setText(QDir::toNativeSeparators(path));
			SaveSettings();
		}
	});
}

void ReplayBufferAIEditor::LoadSettings()
{
	auto s = Settings();
	enabled_->setChecked(s.value(QStringLiteral("enabled"), true).toBool());
	autoReplay_->setChecked(s.value(QStringLiteral("autoReplay"), true).toBool());
	captions_->setChecked(s.value(QStringLiteral("captions"), true).toBool());
	worker_->setText(s.value(QStringLiteral("worker"), QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("LocalEditorWorker.exe"))).toString());
	prompt_->setText(s.value(QStringLiteral("prompt")).toString());
	auto setData = [&s](QComboBox *box, const QString &key, const QString &fallback) {
		const QString value = s.value(key, fallback).toString();
		const int index = box->findData(value);
		box->setCurrentIndex(index >= 0 ? index : 0);
	};
	setData(style_, QStringLiteral("style"), QStringLiteral("viral_gaming"));
	setData(social_, QStringLiteral("social"), QStringLiteral("tiktok_9x16"));
	setData(reframe_, QStringLiteral("reframe"), QStringLiteral("smart_crop"));
	setData(color_, QStringLiteral("color"), QStringLiteral("vibrant"));
	setData(smoothFps_, QStringLiteral("smoothFps"), QStringLiteral("native"));
}

void ReplayBufferAIEditor::SaveSettings()
{
	auto s = Settings();
	s.setValue(QStringLiteral("enabled"), enabled_->isChecked());
	s.setValue(QStringLiteral("autoReplay"), autoReplay_->isChecked());
	s.setValue(QStringLiteral("captions"), captions_->isChecked());
	s.setValue(QStringLiteral("worker"), worker_->text().trimmed());
	s.setValue(QStringLiteral("prompt"), prompt_->text());
	s.setValue(QStringLiteral("style"), style_->currentData());
	s.setValue(QStringLiteral("social"), social_->currentData());
	s.setValue(QStringLiteral("reframe"), reframe_->currentData());
	s.setValue(QStringLiteral("color"), color_->currentData());
	s.setValue(QStringLiteral("smoothFps"), smoothFps_->currentData());
	s.sync();
	status_->setText(enabled_->isChecked() ? tr("AI settings applied.") : tr("Local AI editing disabled."));
	DispatchNext();
}

QString ReplayBufferAIEditor::JobsRoot() const
{
	const QString path = QDir(AppDataReplayBuffer()).filePath(QStringLiteral("Automation/jobs"));
	QDir().mkpath(path);
	return path;
}

QString ReplayBufferAIEditor::ExportRoot() const
{
	QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (videos.isEmpty())
		videos = QDir::homePath();
	const QString path = QDir(videos).filePath(QStringLiteral("ReplayBuffer Edits"));
	QDir().mkpath(path);
	return path;
}

QString ReplayBufferAIEditor::WorkerPath() const
{
	return QDir::fromNativeSeparators(worker_->text().trimmed());
}

bool ReplayBufferAIEditor::WriteJsonAtomic(const QString &path, const QJsonObject &object) const
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0)
		return false;
	return file.commit();
}

void ReplayBufferAIEditor::QueuePath(const QString &rawPath, bool automatic, const QString &reason)
{
	const QString sourcePath = QFileInfo(rawPath).absoluteFilePath();
	if (!enabled_->isChecked()) {
		status_->setText(tr("Enable local AI editing first."));
		return;
	}
	if (!QFileInfo::exists(sourcePath)) {
		status_->setText(tr("Source file does not exist: %1").arg(sourcePath));
		return;
	}

	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	const int fps = ovi.fps_den ? qMax(1, static_cast<int>(ovi.fps_num / ovi.fps_den)) : 60;
	const int width = qMax(1, static_cast<int>(ovi.output_width ? ovi.output_width : ovi.base_width));
	const int height = qMax(1, static_cast<int>(ovi.output_height ? ovi.output_height : ovi.base_height));

	const QString preset = social_->currentData().toString();
	const auto target = SocialSize(preset);
	const QString id = QStringLiteral("obs-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(QCoreApplication::applicationPid());
	const QString dir = QDir(JobsRoot()).filePath(id);
	if (!QDir().mkpath(dir)) {
		status_->setText(tr("Could not create AI job directory."));
		return;
	}

	QJsonObject effects{{QStringLiteral("motionBlur"), QStringLiteral("off")},
			    {QStringLiteral("colorTreatment"), color_->currentData().toString()},
			    {QStringLiteral("reframeMode"), reframe_->currentData().toString()},
			    {QStringLiteral("smoothFps"), smoothFps_->currentData().toString()}};
	QJsonObject captions{{QStringLiteral("enabled"), captions_->isChecked()},
			     {QStringLiteral("engine"), QStringLiteral("auto")},
			     {QStringLiteral("language"), QStringLiteral("auto")}};
	QJsonObject video{{QStringLiteral("width"), width}, {QStringLiteral("height"), height}, {QStringLiteral("fps"), fps}};
	QJsonObject planner{{QStringLiteral("goal"), QStringLiteral("professional_vertical_highlight")},
			    {QStringLiteral("targetAspect"), SocialAspect(preset)},
			    {QStringLiteral("targetWidth"), target.first},
			    {QStringLiteral("targetHeight"), target.second},
			    {QStringLiteral("preserveGameplayContinuity"), true},
			    {QStringLiteral("avoidOvercutting"), true},
			    {QStringLiteral("audioFirst"), true},
			    {QStringLiteral("subjectTrackingPreferred"), true},
			    {QStringLiteral("safeFullFrameFallback"), true},
			    {QStringLiteral("smoothReframePreferred"), true},
			    {QStringLiteral("captionWordTimingPreferred"), true},
			    {QStringLiteral("criticPassRequired"), true},
			    {QStringLiteral("minimumCriticScore"), 0.82}};
	QJsonObject job{{QStringLiteral("schema"), 2},
			{QStringLiteral("jobId"), id},
			{QStringLiteral("origin"), QString::fromLatin1(kOrigin)},
			{QStringLiteral("state"), QStringLiteral("queued")},
			{QStringLiteral("localOnly"), true},
			{QStringLiteral("jobType"), QStringLiteral("edit")},
			{QStringLiteral("sourcePath"), sourcePath},
			{QStringLiteral("trigger"), automatic ? QStringLiteral("automatic") : QStringLiteral("manual")},
			{QStringLiteral("reason"), reason},
			{QStringLiteral("app"), QStringLiteral("OBS Studio + ReplayBuffer")},
			{QStringLiteral("prompt"), prompt_->text()},
			{QStringLiteral("style"), style_->currentData().toString()},
			{QStringLiteral("socialPreset"), preset},
			{QStringLiteral("export"), QJsonObject{{QStringLiteral("preset"), preset}}},
			{QStringLiteral("effects"), effects},
			{QStringLiteral("captions"), captions},
			{QStringLiteral("durationMs"), 60000},
			{QStringLiteral("eventOffsetMs"), QJsonValue(QJsonValue::Null)},
			{QStringLiteral("video"), video},
			{QStringLiteral("planner"), planner},
			{QStringLiteral("exportDirectory"), ExportRoot()},
			{QStringLiteral("resultPath"), QDir(dir).filePath(QStringLiteral("result.json"))}};

	if (!WriteJsonAtomic(QDir(dir).filePath(QStringLiteral("job.json")), job)) {
		QDir(dir).removeRecursively();
		status_->setText(tr("Could not write AI job."));
		return;
	}
	status_->setText(tr("Queued AI edit: %1").arg(QFileInfo(sourcePath).fileName()));
	DispatchNext();
	RefreshState();
}

void ReplayBufferAIEditor::QueueLastReplay()
{
	char *raw = obs_frontend_get_last_replay();
	if (!raw || !*raw) {
		if (raw)
			bfree(raw);
		status_->setText(tr("OBS has no saved replay yet."));
		return;
	}
	const QString path = QString::fromUtf8(raw);
	bfree(raw);
	QueuePath(path, false, tr("Manual edit of latest OBS replay"));
}

void ReplayBufferAIEditor::QueueFile()
{
	const QString path = QFileDialog::getOpenFileName(this, tr("Choose recording to edit"), QString(), tr("Video files (*.mkv *.mp4 *.mov *.ts *.flv *.webm);;All files (*.*)"));
	if (!path.isEmpty())
		QueuePath(path, false, tr("Manual file edit"));
}

void ReplayBufferAIEditor::FrontendEvent(enum obs_frontend_event event, void *privateData)
{
	auto *self = static_cast<ReplayBufferAIEditor *>(privateData);
	if (self)
		self->HandleFrontendEvent(event);
}

void ReplayBufferAIEditor::HandleFrontendEvent(enum obs_frontend_event event)
{
	if (event != OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED || !enabled_->isChecked() || !autoReplay_->isChecked())
		return;
	char *raw = obs_frontend_get_last_replay();
	if (!raw || !*raw) {
		if (raw)
			bfree(raw);
		return;
	}
	const QString path = QString::fromUtf8(raw);
	bfree(raw);
	QTimer::singleShot(0, this, [this, path] { QueuePath(path, true, tr("OBS replay saved")); });
}

void ReplayBufferAIEditor::DispatchNext()
{
	if (!enabled_->isChecked() || !workerProcess_ || workerProcess_->state() != QProcess::NotRunning)
		return;
	const QString workerPath = WorkerPath();
	if (!QFileInfo::exists(workerPath))
		return;
	const QStringList pending = PendingJobs(JobsRoot());
	if (pending.isEmpty())
		return;
	const QString jobPath = pending.front();
	activeJobId_ = ReadObject(jobPath).value(QStringLiteral("jobId")).toString();
	workerProcess_->setProperty("jobPath", jobPath);
	workerProcess_->setWorkingDirectory(QFileInfo(workerPath).absolutePath());
	workerProcess_->start(workerPath, {QStringLiteral("--replaybuffer-edit-job"), jobPath});
	if (!workerProcess_->waitForStarted(1000)) {
		status_->setText(tr("Could not launch LocalEditorWorker.exe: %1").arg(workerProcess_->errorString()));
		activeJobId_.clear();
		return;
	}
	status_->setText(tr("AI worker processing %1").arg(activeJobId_));
}

void ReplayBufferAIEditor::RefreshState()
{
	const QString selected = SelectedReadyId();
	int queued = 0;
	int failed = 0;
	QString firstActive;
	QString activeLabel;
	int activePercent = 0;

	QDir root(JobsRoot());
	const auto dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
	for (const QFileInfo &dirInfo : dirs) {
		const QString dir = dirInfo.absoluteFilePath();
		const QJsonObject job = ReadObject(QDir(dir).filePath(QStringLiteral("job.json")));
		if (!IsOwnJob(job))
			continue;
		const QString resultPath = QDir(dir).filePath(QStringLiteral("result.json"));
		if (!QFileInfo::exists(resultPath)) {
			++queued;
			if (firstActive.isEmpty()) {
				firstActive = job.value(QStringLiteral("jobId")).toString();
				const QJsonObject progress = ReadObject(QDir(dir).filePath(QStringLiteral("progress.json")));
				activePercent = qBound(0, static_cast<int>(progress.value(QStringLiteral("percent")).toDouble()), 100);
				activeLabel = progress.value(QStringLiteral("stageLabel")).toString();
				if (activeLabel.isEmpty())
					activeLabel = tr("Queued / starting");
			}
		} else {
			const QJsonObject result = ReadObject(resultPath);
			if (!result.value(QStringLiteral("ok")).toBool(false) && result.value(QStringLiteral("error")).toString() != QStringLiteral("cancelled"))
				++failed;
		}
	}

	activeJobId_ = workerProcess_ && workerProcess_->state() != QProcess::NotRunning ? activeJobId_ : firstActive;
	if (!activeJobId_.isEmpty()) {
		active_->setText(activeLabel.isEmpty() ? tr("Editing %1").arg(activeJobId_) : activeLabel);
		progress_->setValue(activePercent);
		cancel_->setEnabled(true);
	} else {
		active_->setText(tr("No active edit"));
		progress_->setValue(0);
		cancel_->setEnabled(false);
	}

	const auto items = ReadReadyItems(JobsRoot());
	ready_->setUpdatesEnabled(false);
	ready_->clear();
	for (const ReadyItem &item : items) {
		QString label = item.reason.isEmpty() ? QFileInfo(item.outputPath).fileName() : item.reason;
		if (item.rankScore > 0.0)
			label += tr("  •  rank %1").arg(item.rankScore, 0, 'f', 2);
		if (!item.semanticEventType.isEmpty())
			label += tr("  •  %1").arg(item.semanticEventType);
		auto *row = new QListWidgetItem(label, ready_);
		row->setData(Qt::UserRole, item.id);
		row->setToolTip(item.outputPath);
		if (item.id == selected)
			ready_->setCurrentItem(row);
	}
	ready_->setUpdatesEnabled(true);

	if (status_->text().isEmpty() || status_->text() == tr("AI editor idle"))
		status_->setText(tr("%1 queued • %2 ready • %3 failed").arg(queued).arg(items.size()).arg(failed));
}

QString ReplayBufferAIEditor::SelectedReadyId() const
{
	const auto *item = ready_ ? ready_->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

void ReplayBufferAIEditor::OpenSelected()
{
	const QString id = SelectedReadyId();
	if (id.isEmpty())
		return;
	for (const ReadyItem &item : ReadReadyItems(JobsRoot())) {
		if (item.id == id) {
			const QString path = !item.exportPath.isEmpty() && QFileInfo::exists(item.exportPath) ? item.exportPath : item.outputPath;
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
			return;
		}
	}
}

void ReplayBufferAIEditor::RateSelected(bool liked)
{
	const QString id = SelectedReadyId();
	if (id.isEmpty())
		return;
	for (const ReadyItem &item : ReadReadyItems(JobsRoot())) {
		if (item.id != id)
			continue;
		const QString feedbackDir = QDir(AppDataReplayBuffer()).filePath(QStringLiteral("Automation"));
		QDir().mkpath(feedbackDir);
		QFile file(QDir(feedbackDir).filePath(QStringLiteral("feedback.jsonl")));
		if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
			QJsonObject feedback{{QStringLiteral("schema"), 1},
					     {QStringLiteral("atMs"), QDateTime::currentMSecsSinceEpoch()},
					     {QStringLiteral("liked"), liked},
					     {QStringLiteral("semanticEventType"), item.semanticEventType},
					     {QStringLiteral("style"), item.style}};
			file.write(QJsonDocument(feedback).toJson(QJsonDocument::Compact));
			file.write("\n");
			status_->setText(liked ? tr("Preference saved: more edits like this.") : tr("Preference saved: fewer edits like this."));
		}
		return;
	}
}

void ReplayBufferAIEditor::DeleteSelected()
{
	const QString id = SelectedReadyId();
	if (id.isEmpty())
		return;
	if (QMessageBox::question(this, tr("Delete generated edit"), tr("Delete this generated AI edit? The original recording is never deleted.")) != QMessageBox::Yes)
		return;
	const QString dir = QDir(JobsRoot()).filePath(id);
	const QJsonObject job = ReadObject(QDir(dir).filePath(QStringLiteral("job.json")));
	if (!IsOwnJob(job))
		return;
	const QJsonObject result = ReadObject(QDir(dir).filePath(QStringLiteral("result.json")));
	const QString output = result.value(QStringLiteral("outputPath")).toString();
	if (!output.isEmpty() && QFileInfo(output).absolutePath() == QFileInfo(dir).absoluteFilePath())
		QFile::remove(output);
	QDir(dir).removeRecursively();
	status_->setText(tr("Generated edit deleted; original kept."));
	RefreshState();
}

void ReplayBufferAIEditor::CancelActive()
{
	if (activeJobId_.isEmpty())
		return;
	const QString dir = QDir(JobsRoot()).filePath(activeJobId_);
	QFile file(QDir(dir).filePath(QStringLiteral("cancel.requested")));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write("cancelled from OBS AI Editor\n");
		status_->setText(tr("Cancellation requested."));
	}
}

void ReplayBufferAIEditor::RetryFailed()
{
	int retried = 0;
	QDir root(JobsRoot());
	for (const QFileInfo &dirInfo : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		const QString dir = dirInfo.absoluteFilePath();
		const QJsonObject job = ReadObject(QDir(dir).filePath(QStringLiteral("job.json")));
		if (!IsOwnJob(job))
			continue;
		const QString resultPath = QDir(dir).filePath(QStringLiteral("result.json"));
		const QJsonObject result = ReadObject(resultPath);
		if (!result.isEmpty() && !result.value(QStringLiteral("ok")).toBool(false) && result.value(QStringLiteral("error")).toString() != QStringLiteral("cancelled")) {
			QFile::remove(resultPath);
			QFile::remove(QDir(dir).filePath(QStringLiteral("progress.json")));
			QFile::remove(QDir(dir).filePath(QStringLiteral("cancel.requested")));
			++retried;
		}
	}
	status_->setText(tr("Requeued %1 failed AI job(s).").arg(retried));
	DispatchNext();
}

void ReplayBufferAIEditor::QueueBestMontage()
{
	const auto ready = ReadReadyItems(JobsRoot());
	if (ready.size() < 2) {
		status_->setText(tr("Create at least two ready AI edits before making a montage."));
		return;
	}
	const int count = qMin(5, ready.size());
	const QString preset = social_->currentData().toString();
	const auto target = SocialSize(preset);
	const QString id = QStringLiteral("obs-%1-%2-montage").arg(QDateTime::currentMSecsSinceEpoch()).arg(QCoreApplication::applicationPid());
	const QString dir = QDir(JobsRoot()).filePath(id);
	QDir().mkpath(dir);
	QJsonArray sources;
	for (int i = 0; i < count; ++i) {
		const auto &item = ready[i];
		sources.push_back(QJsonObject{{QStringLiteral("path"), item.outputPath},
					      {QStringLiteral("reason"), item.reason},
					      {QStringLiteral("highlightScore"), item.highlightScore},
					      {QStringLiteral("rankScore"), item.rankScore},
					      {QStringLiteral("fps"), qMax(1, item.fps)},
					      {QStringLiteral("captionsBurnedIn"), item.captionsBurnedIn},
					      {QStringLiteral("style"), item.style.isEmpty() ? style_->currentData().toString() : item.style},
					      {QStringLiteral("semanticEventType"), item.semanticEventType}});
	}
	QJsonObject job{{QStringLiteral("schema"), 2},
			{QStringLiteral("jobId"), id}, {QStringLiteral("origin"), QString::fromLatin1(kOrigin)},
			{QStringLiteral("state"), QStringLiteral("queued")}, {QStringLiteral("localOnly"), true},
			{QStringLiteral("jobType"), QStringLiteral("montage")}, {QStringLiteral("sourcePath"), ready.front().outputPath},
			{QStringLiteral("trigger"), QStringLiteral("manual")}, {QStringLiteral("reason"), tr("Best %1 montage").arg(count)},
			{QStringLiteral("app"), QStringLiteral("OBS Studio + ReplayBuffer")}, {QStringLiteral("style"), style_->currentData().toString()},
			{QStringLiteral("socialPreset"), preset}, {QStringLiteral("export"), QJsonObject{{QStringLiteral("preset"), preset}}},
			{QStringLiteral("effects"), QJsonObject{{QStringLiteral("smoothFps"), smoothFps_->currentData().toString()}}},
			{QStringLiteral("durationMs"), 30000}, {QStringLiteral("eventOffsetMs"), QJsonValue(QJsonValue::Null)},
			{QStringLiteral("video"), QJsonObject{{QStringLiteral("width"), target.first}, {QStringLiteral("height"), target.second}, {QStringLiteral("fps"), 60}}},
			{QStringLiteral("sources"), sources}, {QStringLiteral("exportDirectory"), ExportRoot()},
			{QStringLiteral("planner"), QJsonObject{{QStringLiteral("goal"), QStringLiteral("best_clips_montage")}, {QStringLiteral("targetAspect"), SocialAspect(preset)}, {QStringLiteral("targetWidth"), target.first}, {QStringLiteral("targetHeight"), target.second}, {QStringLiteral("targetDurationMs"), 30000}, {QStringLiteral("minimumCriticScore"), 0.82}}},
			{QStringLiteral("resultPath"), QDir(dir).filePath(QStringLiteral("result.json"))}};
	if (!WriteJsonAtomic(QDir(dir).filePath(QStringLiteral("job.json")), job)) {
		QDir(dir).removeRecursively();
		status_->setText(tr("Could not create montage job."));
		return;
	}
	status_->setText(tr("Best-clips montage queued from %1 edits.").arg(count));
	DispatchNext();
}

void ReplayBufferAIEditor::CheckRuntime()
{
	const QString path = WorkerPath();
	if (!QFileInfo::exists(path)) {
		runtime_->setText(tr("Runtime: LocalEditorWorker.exe not found. Browse to it or build/package it beside OBS."));
		return;
	}
	if (doctorProcess_->state() != QProcess::NotRunning)
		return;
	runtime_->setText(tr("Runtime: checking…"));
	doctorProcess_->setWorkingDirectory(QFileInfo(path).absolutePath());
	doctorProcess_->start(path, {QStringLiteral("--replaybuffer-worker-doctor")});
}

void ReplayBufferAIEditor::SetupRuntime()
{
	const QString workerDir = QFileInfo(WorkerPath()).absolutePath();
	const QString script = QDir(workerDir).filePath(QStringLiteral("editor/setup_test_worker.ps1"));
	if (!QFileInfo::exists(script)) {
		runtime_->setText(tr("Runtime setup script is missing beside the worker."));
		return;
	}
	const bool ok = QProcess::startDetached(QStringLiteral("powershell.exe"),
		{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script, QStringLiteral("-InstallCoreTools")}, workerDir);
	runtime_->setText(ok ? tr("Runtime setup launched in PowerShell.") : tr("Could not launch runtime setup."));
}

#endif

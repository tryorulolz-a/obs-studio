#pragma once

#ifdef _WIN32

#include <obs-frontend-api.h>
#include <QJsonObject>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProcess;
class QProgressBar;
class QPushButton;
class QTimer;

class ReplayBufferAIEditor final : public QWidget {
public:
	explicit ReplayBufferAIEditor(QWidget *parent = nullptr);
	~ReplayBufferAIEditor() override;

private:
	static void FrontendEvent(enum obs_frontend_event event, void *privateData);
	void HandleFrontendEvent(enum obs_frontend_event event);
	void BuildUi();
	void LoadSettings();
	void SaveSettings();
	void RefreshState();
	void DispatchNext();
	void QueuePath(const QString &path, bool automatic, const QString &reason);
	void QueueLastReplay();
	void QueueFile();
	void QueueBestMontage();
	void CheckRuntime();
	void SetupRuntime();
	void RetryFailed();
	void CancelActive();
	void OpenSelected();
	void RateSelected(bool liked);
	void DeleteSelected();
	QString SelectedReadyId() const;
	QString JobsRoot() const;
	QString WorkerPath() const;
	QString ExportRoot() const;
	bool WriteJsonAtomic(const QString &path, const QJsonObject &object) const;

	QCheckBox *enabled_ = nullptr;
	QCheckBox *autoReplay_ = nullptr;
	QCheckBox *captions_ = nullptr;
	QComboBox *style_ = nullptr;
	QComboBox *social_ = nullptr;
	QComboBox *reframe_ = nullptr;
	QComboBox *color_ = nullptr;
	QComboBox *smoothFps_ = nullptr;
	QLineEdit *prompt_ = nullptr;
	QLineEdit *worker_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *runtime_ = nullptr;
	QLabel *active_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QListWidget *ready_ = nullptr;
	QPushButton *cancel_ = nullptr;
	QTimer *timer_ = nullptr;
	QProcess *workerProcess_ = nullptr;
	QProcess *doctorProcess_ = nullptr;
	QString activeJobId_;
};

#endif

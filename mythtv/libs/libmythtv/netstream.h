/* Network stream
 * Copyright 2011 Lawrence Rust <lvr at softsystem dot co dot uk>
 */
#ifndef NETSTREAM_H
#define NETSTREAM_H

#include <QList>
#include <QString>
#include <QByteArray>
#include <QObject>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslError>
#include <QWaitCondition>
#include <QQueue>
#include <QDateTime>

class QUrl;
class QNetworkAccessManager;
class NetStreamRequest;
class NetStreamAbort;


/**
 * Stream content from a URI
 */
class NetStream : public QObject
{
    Q_OBJECT

public:
    enum EMode { kNeverCache, kPreferCache, kAlwaysCache };
    NetStream(const QUrl &, EMode mode = kPreferCache,
              const QByteArray &cert = QByteArray());
    virtual ~NetStream();

public:
    // RingBuffer interface
    static bool IsSupported(const QUrl &);
    bool IsOpen() const;
    void Abort();
    int safe_read(void *data, unsigned size, unsigned millisecs = 0);
    qlonglong Seek(qlonglong);
    qlonglong GetReadPosition() const;
    qlonglong GetSize() const;

    // Properties
    const QUrl &Url() const { return m_url; }

    // Synchronous interface
    bool WaitTillReady(unsigned long millisecs);
    bool WaitTillFinished(unsigned long millisecs);
    QNetworkReply::NetworkError GetError() const;
    QString GetErrorString() const;
    qlonglong BytesAvailable() const;
    QByteArray ReadAll();

    // Async interface
    bool isStarted() const;
    bool isReady() const;
    bool isFinished() const;

signals:
    void ReadyRead(QObject*);
    void Finished(QObject*);

public:
    // Time when a URI was last written to cache or invalid if not cached.
    static QDateTime GetLastModified(const QUrl &url);
    // Is the network accessible
    static bool isAvailable();

    // Implementation
private slots:
    // NAMThread signals
    void slotRequestStarted(int, QNetworkReply *);
    // QNetworkReply signals
    void slotFinished();
#ifndef QT_NO_OPENSSL
    void slotSslErrors(const QList<QSslError> & errors);
#endif
    // QIODevice signals
    void slotReadyRead();

private:
    Q_DISABLE_COPY(NetStream)

    bool Request(const QUrl &);

    const int m_id; // Unique request ID
    const QUrl m_url;

    mutable QMutex m_mutex; // Protects r/w access to the following data
    QNetworkRequest m_request;
    enum { kClosed, kPending, kStarted, kReady, kFinished } m_state;
    NetStreamRequest* m_pending;
    QNetworkReply* m_reply;
    int m_nRedirections;
    qlonglong m_size;
    qlonglong m_pos;
    QByteArray m_cert;
    QWaitCondition m_ready;
    QWaitCondition m_finished;
};


/**
 * Thread to process NetStream requests
 */
class NAMThread : public QThread
{
    Q_OBJECT

    // Use manager() to create
    NAMThread();

public:
    static NAMThread & manager(); // Singleton
    virtual ~NAMThread();

    static inline void PostEvent(QEvent *e) { manager().Post(e); }
    void Post(QEvent *event);

    static inline QMutex* GetMutex() { return &manager().m_mutexNAM; }

    static bool isAvailable(); // is network usable
    static QDateTime GetLastModified(const QUrl &url);

signals:
     void requestStarted(int, QNetworkReply *);

    // Implementation
protected:
    void run() override; // QThread
    bool NewRequest(QEvent *);
    bool StartRequest(NetStreamRequest *);
    bool AbortRequest(NetStreamAbort *);

private slots:
    void quit();

private:
    Q_DISABLE_COPY(NAMThread)

    volatile bool m_bQuit;
    QSemaphore m_running;
    mutable QMutex m_mutexNAM; // Provides recursive access to m_nam
    QNetworkAccessManager *m_nam;
    mutable QMutex m_mutex; // Protects r/w access to the following data
    QQueue< QEvent * > m_workQ;
    QWaitCondition m_work;
};

#endif /* ndef NETSTREAM_H */

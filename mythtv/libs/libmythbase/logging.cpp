#include <QAtomicInt>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QList>
#include <QQueue>
#include <QHash>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QMap>
#include <QRegExp>
#include <QVariantMap>
#include <iostream>

using namespace std;

#include "mythlogging.h"
#include "logging.h"
#include "mythdb.h"
#include "mythdirs.h"
#include "mythcorecontext.h"
#include "mythsystemlegacy.h"
#include "mythsignalingtimer.h"
#include "dbutil.h"
#include "exitcodes.h"
#include "compat.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#define SYSLOG_NAMES
#ifndef _WIN32
#include <mythsyslog.h>
#endif
#include <unistd.h>

// Various ways to get to thread's tid
#if defined(linux)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
extern "C" {
#include <sys/ucontext.h>
#include <sys/thr.h>
}
#elif CONFIG_DARWIN
#include <mach/mach.h>
#endif

// QJson
#include "qjsonwrapper/Json.h"

#ifdef Q_OS_ANDROID
#include <android/log.h>
#endif

static QMutex                  logQueueMutex;
static QQueue<LoggingItem *>   logQueue;
static QRegExp                 logRegExp = QRegExp("[%]{1,2}");

static LoggerThread           *logThread = nullptr;
static QMutex                  logThreadMutex;
static QHash<uint64_t, char *> logThreadHash;

static QMutex                   logThreadTidMutex;
static QHash<uint64_t, int64_t> logThreadTidHash;

static bool                    logThreadFinished = false;
static bool                    debugRegistration = false;

typedef struct {
    bool    propagate;
    int     quiet;
    int     facility;
    bool    dblog;
    QString path;
} LogPropagateOpts;

LogPropagateOpts        logPropagateOpts;
QString                 logPropagateArgs;
QStringList             logPropagateArgList;

#define TIMESTAMP_MAX 30
#define MAX_STRING_LENGTH (LOGLINE_MAX+120)

LogLevel_t logLevel = LOG_INFO;

bool verboseInitialized = false;
VerboseMap verboseMap;
QMutex verboseMapMutex;

LoglevelMap loglevelMap;
QMutex loglevelMapMutex;

const uint64_t verboseDefaultInt = VB_GENERAL;
const char    *verboseDefaultStr = " general";

uint64_t verboseMask = verboseDefaultInt;
QString verboseString = QString(verboseDefaultStr);
ComponentLogLevelMap componentLogLevel;

uint64_t     userDefaultValueInt = verboseDefaultInt;
QString      userDefaultValueStr = QString(verboseDefaultStr);
bool         haveUserDefaultValues = false;

void verboseAdd(uint64_t mask, QString name, bool additive, QString helptext);
void loglevelAdd(int value, QString name, char shortname);
void verboseInit(void);
void verboseHelp(void);

void loggingGetTimeStamp(qlonglong *epoch, uint *usec)
{
#if HAVE_GETTIMEOFDAY
    struct timeval  tv;
    gettimeofday(&tv, nullptr);
    *epoch = tv.tv_sec;
    if (usec)
        *usec  = tv.tv_usec;
#else
    /* Stupid system has no gettimeofday, use less precise QDateTime */
    QDateTime date = MythDate::current();
    *epoch = date.toTime_t();
    if (usec)
    {
        QTime     time = date.time();
        *usec  = time.msec() * 1000;
    }
#endif
}

LoggingItem::LoggingItem() :
        ReferenceCounter("LoggingItem", false),
        m_pid(-1), m_tid(-1), m_threadId(-1), m_usec(0), m_line(0),
        m_type(kMessage), m_level(LOG_INFO), m_facility(0), m_epoch(0),
        m_file(nullptr), m_function(nullptr), m_threadName(nullptr), m_appName(nullptr),
        m_table(nullptr), m_logFile(nullptr)
{
    m_message[0]='\0';
    m_message[LOGLINE_MAX]='\0';
}

LoggingItem::LoggingItem(const char *_file, const char *_function,
                         int _line, LogLevel_t _level, LoggingType _type) :
        ReferenceCounter("LoggingItem", false), m_pid(-1),
        m_threadId((uint64_t)(QThread::currentThreadId())),
        m_line(_line), m_type(_type), m_level(_level), m_facility(0),
        m_file(strdup(_file)), m_function(strdup(_function)),
        m_threadName(nullptr), m_appName(nullptr), m_table(nullptr), m_logFile(nullptr)
{
    loggingGetTimeStamp(&m_epoch, &m_usec);

    m_message[0]='\0';
    m_message[LOGLINE_MAX]='\0';
    setThreadTid();
}

LoggingItem::~LoggingItem()
{
    free(m_file);

    free(m_function);

    free(m_threadName);

    free(m_appName);

    free(m_table);

    free(m_logFile);
}

QByteArray LoggingItem::toByteArray(void)
{
    QVariantMap variant = QJsonWrapper::qobject2qvariant(this);
    QByteArray json = QJsonWrapper::toJson(variant);

    //cout << json.constData() << endl;

    return json;
}

/// \brief Get the name of the thread that produced the LoggingItem
/// \return C-string of the thread name
char *LoggingItem::getThreadName(void)
{
    static const char  *unknown = "thread_unknown";

    if( m_threadName )
        return m_threadName;

    QMutexLocker locker(&logThreadMutex);
    return logThreadHash.value(m_threadId, (char *)unknown);
}

/// \brief Get the thread ID of the thread that produced the LoggingItem
/// \return Thread ID of the producing thread, cast to a 64-bit signed integer
/// \note  In different platforms, the actual value returned here will vary.
///        The intention is to get a thread ID that will map well to what is
///        shown in gdb.
int64_t LoggingItem::getThreadTid(void)
{
    QMutexLocker locker(&logThreadTidMutex);
    m_tid = logThreadTidHash.value(m_threadId, 0);
    return m_tid;
}

/// \brief Set the thread ID of the thread that produced the LoggingItem.  This
///        code is actually run in the thread in question as part of the call
///        to LOG()
/// \note  In different platforms, the actual value returned here will vary.
///        The intention is to get a thread ID that will map well to what is
///        shown in gdb.
void LoggingItem::setThreadTid(void)
{
    QMutexLocker locker(&logThreadTidMutex);

    m_tid = logThreadTidHash.value(m_threadId, -1);
    if (m_tid == -1)
    {
        m_tid = 0;

#if defined(Q_OS_ANDROID)
        m_tid = (int64_t)gettid();
#elif defined(linux)
        m_tid = syscall(SYS_gettid);
#elif defined(__FreeBSD__)
        long lwpid;
        int dummy = thr_self( &lwpid );
        (void)dummy;
        m_tid = (int64_t)lwpid;
#elif CONFIG_DARWIN
        m_tid = (int64_t)mach_thread_self();
#endif
        logThreadTidHash[m_threadId] = m_tid;
    }
}

/// \brief LoggerThread constructor.  Enables debugging of thread registration
///        and deregistration if the VERBOSE_THREADS environment variable is
///        set.
LoggerThread::LoggerThread(QString filename, bool progress, bool quiet,
                           QString table, int facility) :
    MThread("Logger"),
    m_waitNotEmpty(new QWaitCondition()),
    m_waitEmpty(new QWaitCondition()),
    m_aborted(false), m_filename(filename), m_progress(progress),
    m_quiet(quiet), m_appname(QCoreApplication::applicationName()),
    m_tablename(table), m_facility(facility), m_pid(getpid()), m_epoch(0)
{
    char *debug = getenv("VERBOSE_THREADS");
    if (debug != nullptr)
    {
        LOG(VB_GENERAL, LOG_NOTICE,
            "Logging thread registration/deregistration enabled!");
        debugRegistration = true;
    }

    moveToThread(qthread());
}

/// \brief LoggerThread destructor.  Triggers the deletion of all loggers.
LoggerThread::~LoggerThread()
{
    stop();
    wait();

    delete m_waitNotEmpty;
    delete m_waitEmpty;
}

/// \brief Run the logging thread.  This thread reads from the logging queue,
///        and handles distributing the LoggingItems to each logger instance.
///        The thread will not exit until the logging queue is emptied
///        completely, ensuring that all logging is flushed.
void LoggerThread::run(void)
{
    RunProlog();

    logThreadFinished = false;

    LOG(VB_GENERAL, LOG_INFO, "Added logging to the console");

    bool dieNow = false;

    QMutexLocker qLock(&logQueueMutex);

    while (!m_aborted || !logQueue.isEmpty())
    {
        qLock.unlock();
        qApp->processEvents(QEventLoop::AllEvents, 10);
        qApp->sendPostedEvents(nullptr, QEvent::DeferredDelete);

        qLock.relock();
        if (logQueue.isEmpty())
        {
            m_waitEmpty->wakeAll();
            m_waitNotEmpty->wait(qLock.mutex(), 100);
            continue;
        }

        LoggingItem *item = logQueue.dequeue();
        qLock.unlock();

        fillItem(item);
        handleItem(item);
        logConsole(item);
        item->DecrRef();

        qLock.relock();
    }

    qLock.unlock();

    // This must be before the timer stop below or we deadlock when the timer
    // thread tries to deregister, and we wait for it.
    logThreadFinished = true;

    RunEpilog();

    // cppcheck-suppress knownConditionTrueFalse
    if (dieNow)
    {
        qApp->processEvents();
    }
}

/// \brief  Handles each LoggingItem.  There is a special case for
///         thread registration and deregistration which are also included in
///         the logging queue to keep the thread names in sync with the log
///         messages.
/// \param  item    The LoggingItem to be handled
void LoggerThread::handleItem(LoggingItem *item)
{
    if (item->m_type & kRegistering)
    {
        item->m_tid = item->getThreadTid();

        QMutexLocker locker(&logThreadMutex);
        if (logThreadHash.contains(item->m_threadId))
        {
            char *threadName = logThreadHash.take(item->m_threadId);
            free(threadName);
        }
        logThreadHash[item->m_threadId] = strdup(item->m_threadName);

        if (debugRegistration)
        {
            snprintf(item->m_message, LOGLINE_MAX,
                     "Thread 0x%" PREFIX64 "X (%" PREFIX64
                     "d) registered as \'%s\'",
                     item->m_threadId,
                     item->m_tid,
                     logThreadHash[item->m_threadId]);
        }
    }
    else if (item->m_type & kDeregistering)
    {
        int64_t tid = 0;

        {
            QMutexLocker locker(&logThreadTidMutex);
            if( logThreadTidHash.contains(item->m_threadId) )
            {
                tid = logThreadTidHash[item->m_threadId];
                logThreadTidHash.remove(item->m_threadId);
            }
        }

        QMutexLocker locker(&logThreadMutex);
        if (logThreadHash.contains(item->m_threadId))
        {
            if (debugRegistration)
            {
                snprintf(item->m_message, LOGLINE_MAX,
                         "Thread 0x%" PREFIX64 "X (%" PREFIX64
                         "d) deregistered as \'%s\'",
                         item->m_threadId,
                         (long long int)tid,
                         logThreadHash[item->m_threadId]);
            }
            char *threadName = logThreadHash.take(item->m_threadId);
            free(threadName);
        }
    }
}

/// \brief Process a log message, writing to the console
/// \param item LoggingItem containing the log message to process
bool LoggerThread::logConsole(LoggingItem *item)
{
    char                line[MAX_STRING_LENGTH];

    if (m_quiet || (m_progress && item->m_level > LOG_ERR))
        return false;

    if (!(item->m_type & kMessage))
        return false;

    item->IncrRef();

    if (item->m_type & kStandardIO)
        snprintf( line, MAX_STRING_LENGTH, "%s", item->m_message );
    else
    {
        char   usPart[9];
        char   timestamp[TIMESTAMP_MAX];
        time_t epoch = item->epoch();
        struct tm tm;
        localtime_r(&epoch, &tm);

        strftime( timestamp, TIMESTAMP_MAX-8, "%Y-%m-%d %H:%M:%S",
                  (const struct tm *)&tm );
        snprintf( usPart, 9, ".%06d", (int)(item->m_usec) );
        strcat( timestamp, usPart );
        char shortname;

        {
            QMutexLocker locker(&loglevelMapMutex);
            LoglevelDef *lev = loglevelMap.value(item->m_level, nullptr);
            if (!lev)
                shortname = '-';
            else
                shortname = lev->shortname;
        }

#if CONFIG_DEBUGTYPE
        snprintf( line, MAX_STRING_LENGTH, "%s %c  %s:%d:%s  %s\n", timestamp,
                  shortname, item->m_file, item->m_line, item->m_function, item->m_message );
#else
        snprintf( line, MAX_STRING_LENGTH, "%s %c  %s\n", timestamp,
                  shortname, item->m_message );
#endif
    }

#ifdef Q_OS_ANDROID
    __android_log_print(ANDROID_LOG_INFO, "mfe", line);
#else
    int result = write( 1, line, strlen(line) );
    (void)result;
#endif

    item->DecrRef();

    return true;
}


/// \brief Stop the thread by setting the abort flag after waiting a second for
///        the queue to be flushed.
void LoggerThread::stop(void)
{
    logQueueMutex.lock();
    flush(1000);
    m_aborted = true;
    logQueueMutex.unlock();
    m_waitNotEmpty->wakeAll();
}

/// \brief  Wait for the queue to be flushed (up to a timeout)
/// \param  timeoutMS   The number of ms to wait for the queue to flush
/// \return true if the queue is empty, false otherwise
bool LoggerThread::flush(int timeoutMS)
{
    QTime t;
    t.start();
    while (!m_aborted && !logQueue.isEmpty() && t.elapsed() < timeoutMS)
    {
        m_waitNotEmpty->wakeAll();
        int left = timeoutMS - t.elapsed();
        if (left > 0)
            m_waitEmpty->wait(&logQueueMutex, left);
    }
    return logQueue.isEmpty();
}

void LoggerThread::fillItem(LoggingItem *item)
{
    if (!item)
        return;

    item->setPid(m_pid);
    item->setThreadName(item->getThreadName());
    item->setAppName(m_appname);
    item->setTable(m_tablename);
    item->setLogFile(m_filename);
    item->setFacility(m_facility);
}


/// \brief  Create a new LoggingItem
/// \param  _file   filename of the source file where the log message is from
/// \param  _function source function where the log message is from
/// \param  _line   line number in the source where the log message is from
/// \param  _level  logging level of the message (LogLevel_t)
/// \param  _type   type of logging message
/// \return LoggingItem that was created
LoggingItem *LoggingItem::create(const char *_file,
                                 const char *_function,
                                 int _line, LogLevel_t _level,
                                 LoggingType _type)
{
    LoggingItem *item = new LoggingItem(_file, _function, _line, _level, _type);

    return item;
}

LoggingItem *LoggingItem::create(QByteArray &buf)
{
    // Deserialize buffer
    QVariant variant = QJsonWrapper::parseJson(buf);

    LoggingItem *item = new LoggingItem;
    QJsonWrapper::qvariant2qobject(variant.toMap(), item);

    return item;
}


/// \brief  Format and send a log message into the queue.  This is called from
///         the LOG() macro.  The intention is minimal blocking of the caller.
/// \param  mask    Verbosity mask of the message (VB_*)
/// \param  level   Log level of this message (LOG_* - matching syslog levels)
/// \param  file    Filename of source code logging the message
/// \param  line    Line number within the source of log message source
/// \param  function    Function name of the log message source
/// \param  fromQString true if this message originated from QString
/// \param  format  printf format string (when not from QString), log message
///                 (when from QString)
/// \param  ...     printf arguments (when not from QString)
void LogPrintLine( uint64_t mask, LogLevel_t level, const char *file, int line,
                   const char *function, int fromQString,
                   const char *format, ... )
{
    va_list         arguments;

    int type = kMessage;
    type |= (mask & VB_FLUSH) ? kFlush : 0;
    type |= (mask & VB_STDIO) ? kStandardIO : 0;
    LoggingItem *item = LoggingItem::create(file, function, line, level,
                                            (LoggingType)type);
    if (!item)
        return;

    char *formatcopy = nullptr;
    if( fromQString && strchr(format, '%') )
    {
        QString string(format);
        format = strdup(string.replace(logRegExp, "%%").toLocal8Bit()
                              .constData());
        formatcopy = (char *)format;
    }

    va_start(arguments, format);
    vsnprintf(item->m_message, LOGLINE_MAX, format, arguments);
    va_end(arguments);

    if (formatcopy)
        free(formatcopy);

    QMutexLocker qLock(&logQueueMutex);

#if defined( _MSC_VER ) && defined( _DEBUG )
        OutputDebugStringA( item->m_message );
        OutputDebugStringA( "\n" );
#endif

    logQueue.enqueue(item);

    if (logThread && logThreadFinished && !logThread->isRunning())
    {
        while (!logQueue.isEmpty())
        {
            item = logQueue.dequeue();
            qLock.unlock();
            logThread->handleItem(item);
            logThread->logConsole(item);
            item->DecrRef();
            qLock.relock();
        }
    }
    else if (logThread && !logThreadFinished && (type & kFlush))
    {
        logThread->flush();
    }
}


/// \brief Generate the logPropagateArgs global with the latest logging
///        level, mask, etc to propagate to all of the mythtv programs
///        spawned from this one.
void logPropagateCalc(void)
{
    logPropagateArgList.clear();

    QString mask = verboseString.trimmed();
    mask.replace(QRegExp(" "), ",");
    mask.remove(QRegExp("^,"));
    logPropagateArgs = " --verbose " + mask;
    logPropagateArgList << "--verbose" << mask;

    if (logPropagateOpts.propagate)
    {
        logPropagateArgs += " --logpath " + logPropagateOpts.path;
        logPropagateArgList << "--logpath" << logPropagateOpts.path;
    }

    QString name = logLevelGetName(logLevel);
    logPropagateArgs += " --loglevel " + name;
    logPropagateArgList << "--loglevel" << name;

    for (int i = 0; i < logPropagateOpts.quiet; i++)
    {
        logPropagateArgs += " --quiet";
        logPropagateArgList << "--quiet";
    }

    if (logPropagateOpts.dblog)
    {
        logPropagateArgs += " --enable-dblog";
        logPropagateArgList << "--enable-dblog";
    }

#if !defined(_WIN32) && !defined(Q_OS_ANDROID)
    if (logPropagateOpts.facility >= 0)
    {
        const CODE *syslogname;

        for (syslogname = &facilitynames[0];
             (syslogname->c_name &&
              syslogname->c_val != logPropagateOpts.facility); syslogname++);

        logPropagateArgs += QString(" --syslog %1").arg(syslogname->c_name);
        logPropagateArgList << "--syslog" << syslogname->c_name;
    }
#if CONFIG_SYSTEMD_JOURNAL
    else if (logPropagateOpts.facility == SYSTEMD_JOURNAL_FACILITY)
    {
        logPropagateArgs += " --systemd-journal";
        logPropagateArgList << "--systemd-journal";
    }
#endif
#endif
}

/// \brief Check if we are propagating a "--quiet"
/// \return true if --quiet is being propagated
bool logPropagateQuiet(void)
{
    return logPropagateOpts.quiet;
}

/// \brief  Entry point to start logging for the application.  This will
///         start up all of the threads needed.
/// \param  logfile Filename of the logfile to create.  Empty if no file.
/// \param  progress    non-zero if progress output will be sent to the console.
///                     This squelches all messages less important than LOG_ERR
///                     on the console
/// \param  quiet       quiet level requested (squelches all console output)
/// \param  facility    Syslog facility to use.  -1 to disable syslog output
/// \param  level       Minimum logging level to put into the logs
/// \param  dblog       true if database logging is requested
/// \param  propagate   true if the logfile path needs to be propagated to child
///                     processes.
void logStart(QString logfile, int progress, int quiet, int facility,
              LogLevel_t level, bool dblog, bool propagate)
{
    if (logThread && logThread->isRunning())
        return;

    logLevel = level;
    LOG(VB_GENERAL, LOG_NOTICE, QString("Setting Log Level to LOG_%1")
             .arg(logLevelGetName(logLevel).toUpper()));

    logPropagateOpts.propagate = propagate;
    logPropagateOpts.quiet = quiet;
    logPropagateOpts.facility = facility;
    logPropagateOpts.dblog = dblog;

    if (propagate)
    {
        QFileInfo finfo(logfile);
        QString path = finfo.path();
        logPropagateOpts.path = path;
    }

    logPropagateCalc();

    QString table = dblog ? QString("logging") : QString("");

    if (!logThread)
        logThread = new LoggerThread(logfile, progress, quiet, table, facility);

    logThread->start();
}

/// \brief  Entry point for stopping logging for an application
void logStop(void)
{
    if (logThread)
    {
        logThread->stop();
        logThread->wait();
        delete logThread;
        logThread = nullptr;
    }
}

/// \brief  Register the current thread with the given name.  This is triggered
///         by the RunProlog() call in each thread.
/// \param  name    the name of the thread being registered.  This is used for
///                 indicating the thread each log message is coming from.
void loggingRegisterThread(const QString &name)
{
    if (logThreadFinished)
        return;

    QMutexLocker qLock(&logQueueMutex);

    LoggingItem *item = LoggingItem::create(__FILE__, __FUNCTION__,
                                            __LINE__, LOG_DEBUG,
                                            kRegistering);
    if (item)
    {
        item->setThreadName((char *)name.toLocal8Bit().constData());
        logQueue.enqueue(item);
    }
}

/// \brief  Deregister the current thread's name.  This is triggered by the
///         RunEpilog() call in each thread.
void loggingDeregisterThread(void)
{
    if (logThreadFinished)
        return;

    QMutexLocker qLock(&logQueueMutex);

    LoggingItem *item = LoggingItem::create(__FILE__, __FUNCTION__, __LINE__,
                                            LOG_DEBUG,
                                            kDeregistering);
    if (item)
        logQueue.enqueue(item);
}


/// \brief  Map a syslog facility name back to the enumerated value
/// \param  facility    QString containing the facility name
/// \return Syslog facility as enumerated type.  Negative if not found.
int syslogGetFacility(QString facility)
{
#ifdef _WIN32
    LOG(VB_GENERAL, LOG_NOTICE,
        "Windows does not support syslog, disabling" );
    Q_UNUSED(facility);
    return( -2 );
#elif defined(Q_OS_ANDROID)
    LOG(VB_GENERAL, LOG_NOTICE,
        "Android does not support syslog, disabling" );
    Q_UNUSED(facility);
    return( -2 );
#elif defined(Q_OS_ANDROID)
    LOG(VB_GENERAL, LOG_NOTICE,
        "Android does not support syslog, disabling" );
    Q_UNUSED(facility);
    return( -2 );
#else
    const CODE *name;
    int i;
    QByteArray ba = facility.toLocal8Bit();
    char *string = (char *)ba.constData();

    for (i = 0, name = &facilitynames[0];
         name->c_name && (strcmp(name->c_name, string) != 0); i++, name++);

    return( name->c_val );
#endif
}

/// \brief  Map a log level name back to the enumerated value
/// \param  level   QString containing the log level name
/// \return Log level as enumerated type.  LOG_UNKNOWN if not found.
LogLevel_t logLevelGet(QString level)
{
    QMutexLocker locker(&loglevelMapMutex);
    if (!verboseInitialized)
    {
        locker.unlock();
        verboseInit();
        locker.relock();
    }

    for (LoglevelMap::iterator it = loglevelMap.begin();
         it != loglevelMap.end(); ++it)
    {
        LoglevelDef *item = (*it);
        if ( item->name == level.toLower() )
            return (LogLevel_t)item->value;
    }

    return LOG_UNKNOWN;
}

/// \brief  Map a log level enumerated value back to the name
/// \param  level   Enumerated value of the log level
/// \return Log level name.  "unknown" if not found.
QString logLevelGetName(LogLevel_t level)
{
    QMutexLocker locker(&loglevelMapMutex);
    if (!verboseInitialized)
    {
        locker.unlock();
        verboseInit();
        locker.relock();
    }
    LoglevelMap::iterator it = loglevelMap.find((int)level);

    if ( it == loglevelMap.end() )
        return QString("unknown");

    return (*it)->name;
}

/// \brief  Add a verbose level to the verboseMap.  Done at initialization.
/// \param  mask    verbose mask (VB_*)
/// \param  name    name of the verbosity level
/// \param  additive    true if this is to be ORed with other masks.  false if
///                     is will clear the other bits.
/// \param  helptext    Descriptive text for --verbose help output
void verboseAdd(uint64_t mask, QString name, bool additive, QString helptext)
{
    VerboseDef *item = new VerboseDef;

    item->mask = mask;
    // VB_GENERAL -> general
    name.remove(0, 3);
    name = name.toLower();
    item->name = name;
    item->additive = additive;
    item->helpText = helptext;

    verboseMap.insert(name, item);
}

/// \brief  Add a log level to the logLevelMap.  Done at initialization.
/// \param  value       log level enumerated value (LOG_*) - matches syslog
///                     levels
/// \param  name        name of the log level
/// \param  shortname   one-letter short name for output into logs
void loglevelAdd(int value, QString name, char shortname)
{
    LoglevelDef *item = new LoglevelDef;

    item->value = value;
    // LOG_CRIT -> crit
    name.remove(0, 4);
    name = name.toLower();
    item->name = name;
    item->shortname = shortname;

    loglevelMap.insert(value, item);
}

/// \brief Initialize the logging levels and verbose levels.
void verboseInit(void)
{
    QMutexLocker locker(&verboseMapMutex);
    QMutexLocker locker2(&loglevelMapMutex);
    verboseMap.clear();
    loglevelMap.clear();

    // This looks funky, so I'll put some explanation here.  The verbosedefs.h
    // file gets included as part of the mythlogging.h include, and at that
    // time, the normal (without _IMPLEMENT_VERBOSE defined) code case will
    // define the VerboseMask enum.  At this point, we force it to allow us
    // to include the file again, but with _IMPLEMENT_VERBOSE set so that the
    // single definition of the VB_* values can be shared to define also the
    // contents of verboseMap, via repeated calls to verboseAdd()

#undef VERBOSEDEFS_H_
#define _IMPLEMENT_VERBOSE
#include "verbosedefs.h"

    verboseInitialized = true;
}


/// \brief Outputs the Verbose levels and their descriptions
///        (for --verbose help)
void verboseHelp(void)
{
    QString m_verbose = userDefaultValueStr.trimmed();
    m_verbose.replace(QRegExp(" "), ",");
    m_verbose.remove(QRegExp("^,"));

    cerr << "Verbose debug levels.\n"
            "Accepts any combination (separated by comma) of:\n\n";

    for (VerboseMap::Iterator vit = verboseMap.begin();
         vit != verboseMap.end(); ++vit )
    {
        VerboseDef *item = vit.value();
        QString name = QString("  %1").arg(item->name, -15, ' ');
        if (item->helpText.isEmpty())
            continue;
        cerr << name.toLocal8Bit().constData() << " - " <<
                item->helpText.toLocal8Bit().constData() << endl;
    }

    cerr << endl <<
      "The default for this program appears to be: '-v " <<
      m_verbose.toLocal8Bit().constData() << "'\n\n"
      "Most options are additive except for 'none' and 'all'.\n"
      "These two are semi-exclusive and take precedence over any\n"
      "other options.  However, you may use something like\n"
      "'-v none,jobqueue' to receive only JobQueue related messages\n"
      "and override the default verbosity level.\n\n"
      "Additive options may also be subtracted from 'all' by\n"
      "prefixing them with 'no', so you may use '-v all,nodatabase'\n"
      "to view all but database debug messages.\n\n";

    cerr << "The 'global' loglevel is specified with --loglevel, but can be\n"
         << "overridden on a component by component basis by appending "
         << "':level'\n"
         << "to the component.\n"
         << "    For example: -v gui:debug,channel:notice,record\n\n";

    cerr << "Some debug levels may not apply to this program.\n" << endl;
}

/// \brief  Parse the --verbose commandline argument and set the verbose level
/// \param  arg the commandline argument following "--verbose"
/// \return an exit code.  GENERIC_EXIT_OK if all is well.
int verboseArgParse(QString arg)
{
    QString option;
    int     idx;

    if (!verboseInitialized)
        verboseInit();

    QMutexLocker locker(&verboseMapMutex);

    verboseMask = verboseDefaultInt;
    verboseString = QString(verboseDefaultStr);

    if (arg.startsWith('-'))
    {
        cerr << "Invalid or missing argument to -v/--verbose option\n";
        return GENERIC_EXIT_INVALID_CMDLINE;
    }

    QStringList verboseOpts = arg.split(QRegExp("[^\\w:]+",
                                                Qt::CaseInsensitive,
                                                QRegExp::RegExp2));
    for (QStringList::Iterator it = verboseOpts.begin();
         it != verboseOpts.end(); ++it )
    {
        option = (*it).toLower();
        bool reverseOption = false;
        QString optionLevel;

        if (option != "none" && option.startsWith("no"))
        {
            reverseOption = true;
            option = option.right(option.length() - 2);
        }

        if (option == "help")
        {
            verboseHelp();
            return GENERIC_EXIT_INVALID_CMDLINE;
        }
        else if (option == "important")
        {
            cerr << "The \"important\" log mask is no longer valid.\n";
        }
        else if (option == "extra")
        {
            cerr << "The \"extra\" log mask is no longer valid.  Please try "
                    "--loglevel debug instead.\n";
        }
        else if (option == "default")
        {
            if (haveUserDefaultValues)
            {
                verboseMask = userDefaultValueInt;
                verboseString = userDefaultValueStr;
            }
            else
            {
                verboseMask = verboseDefaultInt;
                verboseString = QString(verboseDefaultStr);
            }
        }
        else
        {
            if ((idx = option.indexOf(':')) != -1)
            {
                optionLevel = option.mid(idx + 1);
                option = option.left(idx);
            }

            VerboseDef *item = verboseMap.value(option);

            if (item)
            {
                if (reverseOption)
                {
                    verboseMask &= ~(item->mask);
                    verboseString = verboseString.remove(' ' + item->name);
                    verboseString += " no" + item->name;
                }
                else
                {
                    if (item->additive)
                    {
                        if (!(verboseMask & item->mask))
                        {
                            verboseMask |= item->mask;
                            verboseString += ' ' + item->name;
                        }
                    }
                    else
                    {
                        verboseMask = item->mask;
                        verboseString = item->name;
                    }

                    if (!optionLevel.isEmpty())
                    {
                        LogLevel_t level = logLevelGet(optionLevel);
                        if (level != LOG_UNKNOWN)
                            componentLogLevel[item->mask] = level;
                    }
                }
            }
            else
            {
                cerr << "Unknown argument for -v/--verbose: " <<
                        option.toLocal8Bit().constData() << endl;;
                return GENERIC_EXIT_INVALID_CMDLINE;
            }
        }
    }

    if (!haveUserDefaultValues)
    {
        haveUserDefaultValues = true;
        userDefaultValueInt = verboseMask;
        userDefaultValueStr = verboseString;
    }

    return GENERIC_EXIT_OK;
}

/// \brief Verbose helper function for ENO macro.
/// \param errnum   system errno value
/// \return QString containing the string version of the errno value, plus the
///                 errno value itself.
QString logStrerror(int errnum)
{
    return QString("%1 (%2)").arg(strerror(errnum)).arg(errnum);
}


/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */

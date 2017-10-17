/*
 *  File  : pbzip2.cpp
 *
 *  Title : Parallel BZIP2 (pbzip2)
 *
 *  Author: Jeff Gilchrist (http://gilchrist.ca/jeff/)
 *           - Modified producer/consumer threading code from
 *             Andrae Muys <andrae@humbug.org.au.au>
 *           - uses libbzip2 by Julian Seward (http://sources.redhat.com/bzip2/)
 *           - Major contributions by Yavor Nikolov (http://javornikolov.wordpress.com)
 */

#include "pbzip2.h"
#include "BZ2StreamScanner.h"
#include "ErrorContext.h"

#include <vector>
#include <algorithm>
#include <string>
#include <new>

extern "C"
{
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <bzlib.h>
#include <limits.h>
}


//
// GLOBALS
//
static int producerDone = 0;
static int terminateFlag = 0; // Abnormal premature termination
static int finishedFlag = 0; // Main thread work finished (about to exit)
static int unfinishedWorkCleaned = 0;
static int numCPU = 2;
static int IgnoreTrailingGarbageFlag = 0; // ingnore trailing garbage on decompress flag
static int SIG_HANDLER_QUIT_SIGNAL = SIGUSR1; // signal used to stop SignalHandlerThread
#ifdef USE_STACKSIZE_CUSTOMIZATION
static int ChildThreadStackSize = 0; // -1 - don't modify stacksize; 0 - use minimum; > 0 - use specified
#ifndef PTHREAD_STACK_MIN
	#define PTHREAD_STACK_MIN 4096
#endif
#endif // USE_STACKSIZE_CUSTOMIZATION
static unsigned char Bz2HeaderZero[] = {
	0x42, 0x5A, 0x68, 0x39, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x00, 0x00, 0x00, 0x00 };
static OFF_T InFileSize;
static int NumBlocks = 0;
static int NumBlocksEstimated = 0;
static int NumBufferedBlocks = 0;
static size_t NumBufferedTailBlocks = 0;
static size_t NumBufferedBlocksMax = 0;
static int NextBlockToWrite;
static int LastGoodBlock; // set only to terminate write prematurely (ignoring garbage)
static int MinErrorBlock; // lowest so far block number which has errors (on decompress; could be trailing garbage) 
static size_t OutBufferPosToWrite; // = 0; // position in output buffer
static int Verbosity = 0;
static int QuietMode = 1;
static int OutputStdOut = 0;
static int ForceOverwrite = 0;
static int BWTblockSize = 9;
static int FileListCount = 0;
static std::vector <outBuff> OutputBuffer;
static queue *FifoQueue; // fifo queue (global var used on termination cleanup)
static pthread_mutex_t *OutMutex = NULL;
static pthread_mutex_t *ProducerDoneMutex = NULL;
static pthread_mutex_t ErrorHandlerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t TerminateFlagMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ProgressIndicatorsMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t *notTooMuchNumBuffered;
static pthread_cond_t TerminateCond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t OutBufferHeadNotEmpty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ErrStateChangeCond = PTHREAD_COND_INITIALIZER;
static pthread_attr_t ChildThreadAttributes;
static struct stat fileMetaData;
static const char *sigInFilename = NULL;
static const char *sigOutFilename = NULL;
static char BWTblockSizeChar = '9';
static sigset_t SignalMask;
static pthread_t SignalHandlerThread;
static pthread_t TerminatorThread;

inline int syncGetProducerDone();
inline void syncSetProducerDone(int newValue);
inline int syncGetTerminateFlag();
inline void syncSetTerminateFlag(int newValue);
inline void syncSetFinishedFlag(int newValue);
inline void syncSetLastGoodBlock(int newValue, int errBlock);
inline int syncGetLastGoodBlock();
void cleanupUnfinishedWork();
void cleanupAndQuit(int exitCode);
int initSignalMask();
int setupSignalHandling();
int setupTerminator();

inline void safe_mutex_lock(pthread_mutex_t *mutex);
inline void safe_mutex_unlock(pthread_mutex_t *mutex);
inline void safe_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
inline void safe_cond_signal(pthread_cond_t *cond);
int safe_cond_timed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, int seconds, const char *caller = "safe_cond_timed_wait");

template <typename FI1, typename FI2>
FI1 memstr(FI1 searchBuf, int searchBufSize, FI2 searchString, int searchStringSize);
int producer_decompress(int, OFF_T, queue *);
int directcompress(int, OFF_T, int, const char *);
int directdecompress(const char *, const char *);
int producer(int hInfile, int blockSize, queue *fifo);
int mutexesInit();
void mutexesDelete();
queue *queueInit(int);
void queueDelete(queue *);
void outputBufferInit(size_t size);
outBuff * outputBufferAdd(const outBuff & element, const char *caller);
outBuff * outputBufferSeqAddNext(outBuff * preveElement, outBuff * newElement);
inline size_t getOutputBufferPos(int blockNum);
int getFileMetaData(const char *);
int writeFileMetaData(const char *);
int testBZ2ErrorHandling(int, BZFILE *, int);
int testCompressedData(char *);
ssize_t bufread(int hf, char *buf, size_t bsize);
int detectCPUs(void);

inline bool isIgnoredTrailingGarbage();
int waitForPreviousBlock(int blockNumToWait, int errBlockNumber);
inline int getLastGoodBlockBeforeErr(int errBlockNumber, int outSequenceNumber);
inline int issueDecompressError(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm, const char * errmsg,
	int exitCode);
int decompressErrCheckSingle(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm, const char * errmsg,
	bool isTrailingGarbageErr);
int decompressErrCheck(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm);
inline bool hasTrailingGarbage(int bzret, const outBuff * fileData,
	const bz_stream & strm);
int producerDecompressCheckInterrupt(int hInfile, outBuff *& fileData, int lastBlock);

using pbzip2::ErrorContext;

/*
 * Pointers to functions used by plain C pthreads API require C calling
 * conventions.
 */
extern "C"
{
void* signalHandlerProc(void* arg);
void* terminatorThreadProc(void* arg);
void *consumer_decompress(void *);
void *fileWriter(void *);
void *consumer(void *);
}

/*
 * Lock mutex or exit application immediately on error.
 */
inline void safe_mutex_lock(pthread_mutex_t *mutex)
{
	int ret = pthread_mutex_lock(mutex);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_mutex_lock error [%d]! Aborting immediately!\n", ret);
		cleanupAndQuit(-5);
	}
}

/*
 * Unlock mutex or exit application immediately on error.
 */
inline void safe_mutex_unlock(pthread_mutex_t *mutex)
{
	int ret = pthread_mutex_unlock(mutex);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_mutex_unlock error [%d]! Aborting immediately!\n", ret);
		cleanupAndQuit(-6);
	}
}

/*
 * Call pthread_cond_signal - check return code and exit application immediately
 * on error.
 */
inline void safe_cond_signal(pthread_cond_t *cond)
{
	int ret = pthread_cond_signal(cond);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_cond_signal error [%d]! Aborting immediately!\n", ret);
		cleanupAndQuit(-7);
	}
}

/*
 * Call pthread_cond_signal - check return code and exit application immediately
 * on error.
 */
inline void safe_cond_broadcast(pthread_cond_t *cond)
{
	int ret = pthread_cond_broadcast(cond);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_cond_broadcast error [%d]! Aborting immediately!\n", ret);
		cleanupAndQuit(-7);
	}
}

/*
 * Unlock mutex or exit application immediately on error.
 */
inline void safe_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	int ret = pthread_cond_wait(cond, mutex);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_cond_wait error [%d]! Aborting immediately!\n", ret);
		pthread_mutex_unlock(mutex);
		cleanupAndQuit(-8);
	}
}

/*
 * Delegate to pthread_cond_timedwait. Check for errors and abort if
 * any encountered. Return 0 on success and non-zero code on error
 */
int safe_cond_timed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, int seconds, const char *caller)
{
	struct timespec waitTimer;
	#ifndef WIN32
	struct timeval tv;
	struct timezone tz;
	#else
	SYSTEMTIME systemtime;
	LARGE_INTEGER filetime;
	#endif

	#ifndef WIN32
	gettimeofday(&tv, &tz);
	waitTimer.tv_sec = tv.tv_sec + seconds;
	waitTimer.tv_nsec = tv.tv_usec * 1000;
	#else
	GetSystemTime(&systemtime);
	SystemTimeToFileTime(&systemtime, (FILETIME *)&filetime);
	waitTimer.tv_sec = filetime.QuadPart / 10000000;
	waitTimer.tv_nsec = filetime.QuadPart - ((LONGLONG)waitTimer.tv_sec * 10000000) * 10;
	waitTimer.tv_sec += seconds;
	#endif
	#ifdef PBZIP_DEBUG
	fprintf(stderr, "%s:  waitTimer.tv_sec: %"PRIiMAX"  waitTimer.tv_nsec: %"PRIiMAX"\n", caller,
		(intmax_t)waitTimer.tv_sec, (intmax_t)waitTimer.tv_nsec);
	#endif
	int pret = pthread_cond_timedwait(cond, mutex, &waitTimer);
	// we are not using a compatible pthreads library so abort
	if ((pret != 0) && (pret != EINTR) && (pret != EBUSY) && (pret != ETIMEDOUT))
	{
		ErrorContext::getInstance()->saveError();
		pthread_mutex_unlock(mutex);
		handle_error(EF_EXIT, 1,
				"pbz2: *ERROR: %s:  pthread_cond_timedwait() call invalid [pret=%d].  This machine\n"
				"         does not have compatible pthreads library.  Aborting.\n", caller, pret);

		cleanupAndQuit(-9);
	}
	#ifdef PBZIP_DEBUG
	else if (pret != 0)
	{
		fprintf(stderr, "%s: pthread_cond_timedwait returned with non-fatal error [%d]\n", caller, pret);
	}
	#endif // PBZIP_DEBUG

	return 0;
}

/*
 * Delegate to write but keep writing until count bytes are written or
 * error is encountered (on success all count bytes would be written)
 */
ssize_t do_write(int fd, const void *buf, size_t count)
{
	ssize_t bytesRemaining = count;
	ssize_t nbytes = 0;
	const char *pbuf = (const char *)buf;
	while ((bytesRemaining > 0) && ((nbytes = write(fd, pbuf, bytesRemaining)) > 0))
	{
		bytesRemaining -= nbytes;
		pbuf += nbytes;
	}

	if (nbytes < 0)
	{
		ErrorContext::getInstance()->saveError();
		return nbytes;
	}

	return (count - bytesRemaining);
}

/*
 * Delegate to read but keep writing until count bytes are read or
 * error is encountered (on success all count bytes would be read)
 */
ssize_t do_read(int fd, void *buf, size_t count)
{
	ssize_t bytesRemaining = count;
	ssize_t nbytes = 0;
	char *pbuf = (char *)buf;
	while ((bytesRemaining > 0) && (nbytes = read(fd, pbuf, bytesRemaining)) > 0)
	{
		bytesRemaining -= nbytes;
		pbuf += nbytes;
	}

	if (nbytes < 0)
	{
		ErrorContext::getInstance()->saveError();
		return nbytes;
	}

	return (count - bytesRemaining);
}

/*
 * Open output file with least required privileges
 */
int safe_open_output(const char *path)
{
	int ret = open(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, FILE_MODE);
	if (ret == -1)
	{
		ErrorContext::getInstance()->saveError();
	}
	
	return ret;
}

/*
 * Based on bzip2.c code
 */
FILE *safe_fopen_output(const char *path, const char *mode)
{
	int fh = safe_open_output(path);
	if (fh == -1)
	{
		return NULL;
	}
	
	FILE *fp = fdopen(fh, mode);
	if (fp == NULL)
	{
		ErrorContext::getInstance()->saveError();
		close(fh);
	}

	return fp;
}

/**
 * Save the given file and save errno on failure.
 * 
 * @param fd file to close
 * @return -1 on failure or 0 on success.
 */
inline int do_close(int fd)
{
	int ret = close(fd);
	if (ret == -1)
	{
		ErrorContext::getInstance()->saveError();
	}
	
	return ret;
}

inline int do_fclose(FILE *file)
{
	int ret = fclose(file);
	if ( ret == EOF )
	{
		ErrorContext::getInstance()->saveError();
	}
	
	return ret;
}

inline int do_fflush(FILE *file)
{
	int ret = fflush(file);
	if ( ret == EOF )
	{
		ErrorContext::getInstance()->saveError();
	}
	
	return ret;
}

/**
 * Close the given file. In case of error - save errno and print error message.
 * 
 * @param file file to close
 * @param fileName name of file to print in case of failure
 * @return fclose return code
 */
inline int verbose_fclose(FILE *file, const char *fileName)
{
	int ret;
	if ( (ret = fclose(file)) == EOF )
	{
		ErrorContext::syncPrintErrnoMsg(stderr, errno);
		fprintf(stderr, "pbz2: *ERROR: Failed to close file [%s]!\n", fileName);
	}
	
	return ret;
}

int do_remove(const char* pathname)
{
	int ret = remove(pathname);
	if (ret == -1)
	{
		ErrorContext::getInstance()->saveError();
	}
	
	return ret;
}

/**
 * Check if a given file exists.
 *
 * @return true if file exists and false if it doesn't
 */
bool check_file_exists( const char * filename )
{
	int hOutfile = open( filename, O_RDONLY | O_BINARY );
	
	if ( hOutfile == -1 )
	{
		ErrorContext::getInstance()->saveError();
		return false;
	}
	else
	{
		close( hOutfile );
		return true;
	}
}

/*
 *********************************************************
	Atomically get producerDone value.
*/
inline int syncGetProducerDone()
{
	int ret;
	safe_mutex_lock(ProducerDoneMutex);
	ret = producerDone;
	safe_mutex_unlock(ProducerDoneMutex);

	return ret;
}

/*
 *********************************************************
	Atomically set producerDone value.
*/
inline void syncSetProducerDone(int newValue)
{
	safe_mutex_lock(ProducerDoneMutex);
	producerDone = newValue;
	safe_mutex_unlock(ProducerDoneMutex);
}

/*
 * Atomic get terminateFlag
 */
inline int syncGetTerminateFlag()
{
	int ret;
	safe_mutex_lock(&TerminateFlagMutex);
	ret = terminateFlag;
	safe_mutex_unlock(&TerminateFlagMutex);

	return ret;
}

/*
 * Atomically set termination flag and signal the related
 * condition.
 */
inline void syncSetTerminateFlag(int newValue)
{
	safe_mutex_lock(&TerminateFlagMutex);

	terminateFlag = newValue;
	if (terminateFlag != 0)
	{
		// wake up terminator thread
		pthread_cond_signal(&TerminateCond);
		safe_mutex_unlock(&TerminateFlagMutex);

		// wake up all other possibly blocked on cond threads
		safe_mutex_lock(OutMutex);
		pthread_cond_broadcast(notTooMuchNumBuffered);
		safe_mutex_unlock(OutMutex);
		if (FifoQueue != NULL)
		{
			safe_mutex_lock(FifoQueue->mut);
			pthread_cond_broadcast(FifoQueue->notFull);
			pthread_cond_broadcast(FifoQueue->notEmpty);
			safe_mutex_unlock(FifoQueue->mut);
		}
	}
	else
	{
		safe_mutex_unlock(&TerminateFlagMutex);
	}
}

/*
 *  Set finishedSucessFlag and signal the related condition.
 */
inline void syncSetFinishedFlag(int newValue)
{
	safe_mutex_lock(&TerminateFlagMutex);

	finishedFlag = newValue;
	if (finishedFlag != 0)
	{
		pthread_cond_signal(&TerminateCond);
	}

	safe_mutex_unlock(&TerminateFlagMutex);
}

/**
 * Set last block which is maybe good (not guaranteed) and MinErrorBlock
 * (lowest block with errors encountered so far).
 * Only moving downwards has effect (attempts to raise the block numbers are ignored).
 * -1 means infinity.
 * 
 * 
 * @param newValue last block which is maybe good. -1 means +infinity.
 * @param errBlock block number which has errors
 */
inline void syncSetLastGoodBlock(int newValue, int errBlock)
{
	bool changed = false;
	
	safe_mutex_lock(OutMutex);
	#ifdef PBZIP_DEBUG
	uintmax_t thid = (uintmax_t) pthread_self();
	fprintf(stderr, "(%"PRIuMAX") syncSetLastGoodBlock: %d -> %d; MinErrorBlock: %d -> %d\n",
		 thid, LastGoodBlock, newValue, MinErrorBlock, errBlock);
	#endif

	if ( (LastGoodBlock == -1) || (newValue < LastGoodBlock) )
	{
		LastGoodBlock = newValue;
		changed = true;
	}
	
	if ( (MinErrorBlock == -1) || (errBlock < MinErrorBlock) )
	{
		MinErrorBlock = errBlock;
		changed = true;
	}

	if ( changed )
	{
		safe_cond_signal(&ErrStateChangeCond);
		safe_cond_signal(&OutBufferHeadNotEmpty);

		// wake up all other possibly blocked on cond threads
		pthread_cond_broadcast(notTooMuchNumBuffered);
		safe_mutex_unlock(OutMutex);

		if (FifoQueue != NULL)
		{
			safe_mutex_lock(FifoQueue->mut);
			pthread_cond_broadcast(FifoQueue->notFull);
			pthread_cond_broadcast(FifoQueue->notEmpty);
			safe_mutex_unlock(FifoQueue->mut);
		}
	}
	else
	{
		safe_mutex_unlock(OutMutex);
	}
}

inline int syncGetLastGoodBlock()
{
	int ret;
	safe_mutex_lock(OutMutex);
	ret = LastGoodBlock;
	safe_mutex_unlock(OutMutex);

	return ret;
}

inline int syncGetMinErrorBlock()
{
	int ret;
	safe_mutex_lock(OutMutex);
	ret = MinErrorBlock;
	safe_mutex_unlock(OutMutex);

	return ret;
}

inline bool isIgnoredTrailingGarbage()
{
	return (IgnoreTrailingGarbageFlag != 0);
}

/*
 *********************************************************
	Print error message and optionally exit or abort
    depending on exitFlag:
     0 - don't quit;
     1 - exit;
     2 - abort.
    On exit - exitCode status is used.
*/
int handle_error(ExitFlag exitFlag, int exitCode, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	ErrorContext::getInstance()->printErrorMessages(stderr);
	fflush(stderr);
	va_end(args);

	if (exitFlag == EF_ABORT)
	{
		syncSetTerminateFlag(1);
		abort();
	}
	if (exitFlag == EF_EXIT)
	{
		syncSetTerminateFlag(1);
	}

	return exitCode;
}

/**
 *
 * @return -1 - terminate flag set (error)
 *          0 - prev block is OK (i.e. we're on the first error here)
 *          2 - lower block number already in error state
 */
int waitForPreviousBlock(int blockNumToWait, int errBlockNumber)
{
	#ifdef PBZIP_DEBUG
	uintmax_t thid = (uintmax_t) pthread_self();
	safe_mutex_lock(OutMutex);
	fprintf( stderr, "(%"PRIuMAX") waitForPreviousBlock enter: LastGoodBlock=%d"
		"; blockNumToWait=%d; NextBlockToWrite=%d; MinErrorBlock=%d; errBlockNumber=%d\n",
		thid,
		LastGoodBlock, blockNumToWait, NextBlockToWrite,
		MinErrorBlock, errBlockNumber );
	safe_mutex_unlock(OutMutex);
	#endif

	for (;;)
	{
		if (syncGetTerminateFlag() != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "(%"PRIuMAX") waitForPreviousBlock terminated [%d]: blockNumToWait=%d\n",
				thid, -1, blockNumToWait );
			#endif
			return -1;
		}

		safe_mutex_lock(OutMutex);

		#ifdef PBZIP_DEBUG
		fprintf( stderr, "(%"PRIuMAX") waitForPreviousBlock before check: LastGoodBlock=%d; blockNumToWait=%d; NextBlockToWrite=%d; MinErrorBlock=%d\n",
			thid, LastGoodBlock, blockNumToWait, NextBlockToWrite, MinErrorBlock );
		#endif

		// This check should (min error block) be before next one (next block to write)
		if ( (MinErrorBlock != -1) && (MinErrorBlock < errBlockNumber) )
		{
			#ifdef PBZIP_DEBUG
			fprintf( stderr, "(%"PRIuMAX") waitForPreviousBlock exit [%d]: LastGoodBlock=%d; blockNumToWait=%d; NextBlockToWrite=%d; MinErrorBlock=%d\n",
				thid, 2, LastGoodBlock, blockNumToWait, NextBlockToWrite, MinErrorBlock );
			#endif
			safe_mutex_unlock(OutMutex);
			return 2;
		}
		
		if (errBlockNumber <= NextBlockToWrite)
		{
			#ifdef PBZIP_DEBUG
			fprintf( stderr, "(%"PRIuMAX") waitForPreviousBlock exit [%d]: LastGoodBlock=%d; blockNumToWait=%d; NextBlockToWrite=%d; MinErrorBlock=%d\n",
				thid, 0, LastGoodBlock, blockNumToWait, NextBlockToWrite, MinErrorBlock );
			#endif
			safe_mutex_unlock(OutMutex);
			return 0;
		}

		#ifdef PBZIP_DEBUG
		fprintf( stderr, "(%"PRIuMAX") waitForPreviousBlock to sleep: LastGoodBlock=%d; blockNumToWait=%d; NextBlockToWrite=%d; MinErrorBlock=%d\n",
			thid, LastGoodBlock, blockNumToWait, NextBlockToWrite, MinErrorBlock );
		#endif

		safe_cond_timed_wait(&ErrStateChangeCond, OutMutex, 1, "waitForPreviousBlock");

		safe_mutex_unlock(OutMutex);
	}
}

/**
 * 
 * @param errBlockNumber block from input file
 * @param outSequenceNumber sequence in the output tail for the given block
 * @return Last input block not after the given which possibly (not guaranteed)
 *         resulted in good out blocks.
 *         -1 if such don't exist.
 */
inline int getLastGoodBlockBeforeErr(int errBlockNumber, int outSequenceNumber)
{
	// if we got the error just in the beginning of a bzip2 stream
	if ( outSequenceNumber != -1 )
	{
		return errBlockNumber;
	}
	else
	{
		return errBlockNumber - 1;
	}
}

/**
 * Helper function delegating to handle_error with the relevant error
 * message
 *
 * @param bzret
 * @param fileData
 * @param outSequenceNumber
 * @param strm
 * @param errmsg
 * @param exitCode
 * @return exitCode is returned
 */
inline int issueDecompressError(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm, const char * errmsg,
	int exitCode)
{
	#ifdef PBZIP_DEBUG
	uintmax_t thid = (uintmax_t) pthread_self();
	fprintf(stderr, "(%"PRIuMAX") enter issueDecompressError: msg=%s; ret=%d; block=%d; seq=%d; isLastInSeq=%d; avail_in=%d\n",
		thid,
		errmsg, bzret, fileData->blockNumber,
		outSequenceNumber, (int)fileData->isLastInSequence, strm.avail_in);
	#endif

	handle_error(EF_EXIT, exitCode,
				"pbz2: %s: ret=%d; block=%d; seq=%d; isLastInSeq=%d; avail_in=%d\n",
				errmsg, bzret, fileData->blockNumber,
				outSequenceNumber, (int)fileData->isLastInSequence, strm.avail_in);
	return exitCode;
}

/**
 * Handle an error condition which is either trailing garbage-like one or not.
 *
 *
 * @param bzret
 * @param fileData block from input file
 * @param outSequenceNumber
 * @param strm
 * @param errmsg
 * @param isTrailingGarbageErr
 * @return ret < 0 - fatal error;
 *               0 - OK (no error at all);
 *               1 - first block of ignored trailing garbage;
 *               2 - error already signalled for earlier block
 */
int decompressErrCheckSingle(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm, const char * errmsg,
	bool isTrailingGarbageErr)
{
	int lastGoodBlock = getLastGoodBlockBeforeErr(fileData->blockNumber, outSequenceNumber);

	#ifdef PBZIP_DEBUG
	uintmax_t thid = (uintmax_t) pthread_self();
	fprintf(stderr, "(%"PRIuMAX") enter decompressErrCheckSingle: msg=%s; ret=%d; block=%d; seq=%d; isLastInSeq=%d; avail_in=%d; lastGoodBlock=%d\n",
		thid,
		errmsg, bzret, fileData->blockNumber,
		outSequenceNumber, (int)fileData->isLastInSequence, strm.avail_in, lastGoodBlock);
	#endif

	if ( (lastGoodBlock == -1) || !isIgnoredTrailingGarbage() )
	{
		issueDecompressError(bzret, fileData, outSequenceNumber, strm, errmsg, -1);
		return -1;
	}
	else
	{
		// Cut off larger block numbers
		syncSetLastGoodBlock(lastGoodBlock, fileData->blockNumber);
		// wait until the state of previous block is known
		int prevState = waitForPreviousBlock(lastGoodBlock, fileData->blockNumber);

		if (prevState == 0)
		{
			// we're the first error block

			if (isTrailingGarbageErr)
			{
				// Trailing garbage detected and ignored - not a fatal warning
				fprintf(stderr, "pbz2: *WARNING: Trailing garbage after EOF ignored!\n");
				return 1;
			}
			else
			{
				// the first error is not kind of trailing garbage -> fatal one
				issueDecompressError(bzret, fileData, outSequenceNumber, strm, errmsg, -1);
				return -1;
			}
		}
		else if (prevState == 2)
		{
			// we're not the first error
			return 2;
		}
		else // (prevState == -1)
		{
			// fatal state encountered
			return -1;
		}
	}
}

/**
 * Check if trailing garbage has been identified during the last decompression
 * operation.
 * 
 * @param bzret last bzip2 return code
 * @param fileData should be initialized before calling this
 * @param strm bzip2 library bz_stream
 * @return true if trailing garbage has been detected. false otherwise
 */
inline bool hasTrailingGarbage(int bzret, const outBuff * fileData, const bz_stream & strm)
{
	return (bzret == BZ_STREAM_END) && 
		( (strm.avail_in != 0) || !fileData->isLastInSequence );
}

/**
 *
 * @param bzret
 * @param fileData
 * @param outSequenceNumber
 * @param strm
 * @return ret < 0 - fatal error;
 *               0 - OK (no error at all);
 *               1 - first block of ignored trailing garbage;
 *               2 - error already signalled for earlier block
 */
int decompressErrCheck(int bzret, const outBuff * fileData,
	int outSequenceNumber, const bz_stream & strm)
{
	if ( hasTrailingGarbage( bzret, fileData, strm ) )
	{
		// Potential trailing garbage
		return decompressErrCheckSingle(bzret, fileData, outSequenceNumber, strm,
				"*ERROR during BZ2_bzDecompress - trailing garbage", true);
	}
	else if ( (bzret != BZ_STREAM_END) && (bzret != BZ_OK) )
	{
		return decompressErrCheckSingle(bzret, fileData, outSequenceNumber, strm,
				"*ERROR during BZ2_bzDecompress - failure exit code", false);
	}
	else if ( strm.avail_in != 0 )
	{
		return decompressErrCheckSingle(bzret, fileData, outSequenceNumber, strm,
				"*ERROR unconsumed in after BZ2_bzDecompress loop", false);
	}
	else if ( (bzret != BZ_STREAM_END) && fileData->isLastInSequence )
	{
		return decompressErrCheckSingle(bzret, fileData, outSequenceNumber, strm,
				"*ERROR on decompress - last in segment reached before BZ_STREAM_END",
				false);
	}

	return 0;
}

/*
 * Initialize and set thread signal mask
 */
int initSignalMask()
{
	int ret = 0;
	ret = sigemptyset(&SignalMask);

	ret = sigaddset(&SignalMask, SIGINT) | ret;
	ret = sigaddset(&SignalMask, SIGTERM) | ret;
	ret = sigaddset(&SignalMask, SIGABRT) | ret;
	ret = sigaddset(&SignalMask, SIG_HANDLER_QUIT_SIGNAL) | ret;
	#ifndef WIN32
	ret = sigaddset(&SignalMask, SIGHUP) | ret;
	#endif

	if (ret == 0)
	{
		ret = pthread_sigmask(SIG_BLOCK, &SignalMask, NULL);
	}

	return ret;
}

/*
 * Initialize attributes for child threads.
 * 
 */
int initChildThreadAttributes()
{
	int ret = pthread_attr_init(&ChildThreadAttributes);

	if (ret < 0)
	{
		fprintf(stderr, "Can't initialize thread atrributes [err=%d]! Aborting...\n", ret);
		exit(-1);
	}

	#ifdef USE_STACKSIZE_CUSTOMIZATION
	if (ChildThreadStackSize > 0)
	{
		ret = pthread_attr_setstacksize(&ChildThreadAttributes, ChildThreadStackSize);

		if (ret != 0)
		{
			fprintf(stderr, "Can't set thread stacksize [err=%d]! Countinue with default one.\n", ret);
		}
	}
	#endif // USE_STACKSIZE_CUSTOMIZATION

	return ret;
}

/*
 * Setup and start signal handling.
 */
int setupSignalHandling()
{
	int ret = initSignalMask();

	if (ret == 0)
	{
		ret = pthread_create(&SignalHandlerThread, &ChildThreadAttributes, signalHandlerProc, NULL);
	}

	return ret;
}

/*
 * Setup and start signal handling.
 */
int setupTerminator()
{
	return pthread_create(&TerminatorThread, &ChildThreadAttributes, terminatorThreadProc, NULL );
}

/*
 *********************************************************
 * Clean unfinished work (after error).
 * Deletes output file if such exists and if not using pipes.
 */
void cleanupUnfinishedWork()
{
	if (unfinishedWorkCleaned != 0)
	{
		return;
	}

	struct stat statBuf;
	int ret = 0;

	#ifdef PBZIP_DEBUG
	fprintf(stderr, " Infile: %s   Outfile: %s\n", sigInFilename, sigOutFilename);
	#endif

	// only cleanup files if we did something with them
	if ((sigInFilename == NULL) || (sigOutFilename == NULL) || (OutputStdOut == 1))
	{
		unfinishedWorkCleaned = 1;
		return;
	}

	if (QuietMode != 1)
	{
		fprintf(stderr, "Cleanup unfinished work [Outfile: %s]...\n", sigOutFilename);
	}

	// check to see if input file still exists
	ret = stat(sigInFilename, &statBuf);
	if (ret == 0)
	{
		// only want to remove output file if input still exists
		if (QuietMode != 1)
			fprintf(stderr, "Deleting output file: %s, if it exists...\n", sigOutFilename);
		ret = remove(sigOutFilename);
		if (ret != 0)
		{
			ErrorContext::syncPrintErrnoMsg(stderr, errno);
			fprintf(stderr, "pbz2:  *WARNING: Deletion of output file (apparently) failed.\n");
		}
		else
		{
			fprintf(stderr, "pbz2:  *INFO: Deletion of output file succeeded.\n");
			sigOutFilename = NULL;
		}
	}
	else
	{
		fprintf(stderr, "pbz2:  *WARNING: Output file was not deleted since input file no longer exists.\n");
		fprintf(stderr, "pbz2:  *WARNING: Output file: %s, may be incomplete!\n", sigOutFilename);
	}

	unfinishedWorkCleaned = 1;
}

/*
 *********************************************************
 */

/*
 *********************************************************
 * Terminator thread procedure: looking at terminateFlag
 * and exit application when it's set.
 */
void* terminatorThreadProc(void* arg)
{
	int ret = pthread_mutex_lock(&TerminateFlagMutex);

	if (ret != 0)
	{
		ErrorContext::syncPrintErrnoMsg(stderr, errno);
		fprintf(stderr, "Terminator thread: pthread_mutex_lock error [%d]! Aborting...\n", ret);
		syncSetTerminateFlag(1);
		cleanupAndQuit(1);
	}
	
	while ((finishedFlag == 0) && (terminateFlag == 0))
	{
		ret = pthread_cond_wait(&TerminateCond, &TerminateFlagMutex);
	}

	// Successfull end
	if (finishedFlag != 0)
	{
		ret = pthread_mutex_unlock(&TerminateFlagMutex);
		return NULL;
	}
	
	// Being here implies (terminateFlag != 0)
	ret = pthread_mutex_unlock(&TerminateFlagMutex);

	fprintf(stderr, "Terminator thread: premature exit requested - quitting...\n");
	cleanupAndQuit(1);

	return NULL; // never reachable
}

/*
 *********************************************************
 * Signal handler thread function to hook cleanup on
 * certain signals.
 */
void* signalHandlerProc(void* arg)
{
	int signalCaught;

	// wait for specified in mask signal
	int ret = sigwait(&SignalMask, &signalCaught);

	if (ret != 0)
	{
		fprintf(stderr, "\n *signalHandlerProc - sigwait error: %d\n", ret);
	}
	else if (signalCaught == SIG_HANDLER_QUIT_SIGNAL)
	{
		return NULL;
	}
	else // ret == 0
	{
		fprintf(stderr, "\n *Control-C or similar caught [sig=%d], quitting...\n", signalCaught);
		// Delegating cleanup and termination to Terminator Thread
		syncSetTerminateFlag(1);
	}

	return NULL;
}

/*
 * Cleanup unfinished work (output file) and exit with the given exit code.
 * To be used to quite on error with non-zero exitCode.
 */
void cleanupAndQuit(int exitCode)
{
	// syncSetTerminateFlag(1);
	
	int ret = pthread_mutex_lock(&ErrorHandlerMutex);
	if (ret != 0)
	{
		fprintf(stderr, "Cleanup Handler: Failed to lock ErrorHandlerMutex! May double cleanup...\n");
	}
	cleanupUnfinishedWork();
	pthread_mutex_unlock(&ErrorHandlerMutex);

	exit(exitCode);
}

/*
 *********************************************************
    This function will search the array pointed to by
    searchBuf[] for the string searchString[] and return
    a pointer to the start of the searchString[] if found
    otherwise return NULL if not found.
*/
template <typename FI1, typename FI2>
FI1 memstr(FI1 searchBuf, int searchBufSize, FI2 searchString, int searchStringSize)
{
	FI1 searchBufEnd = searchBuf + searchBufSize;
	FI1 s = std::search(searchBuf, searchBufEnd,
						searchString, searchString + searchStringSize);

	return (s != searchBufEnd) ? s : NULL;
}

/**
 * Check for interrupt conditions - report if any and perform the relevant
 * cleanup
 *
 * @param hInfile
 * @param fileData
 * @param lastBlock
 * @return 0 - not interrupted; 1 - interrupted (terminate flag or other error encountered)
 */
int producerDecompressCheckInterrupt(int hInfile, outBuff *& fileData, int lastBlock)
{
	bool isInterrupted = false;

	if (syncGetLastGoodBlock() != -1)
	{
		isInterrupted = true;

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "producer_decompress: interrupt1 - LastGoodBlock set. "
			"Last produced=%d\n", lastBlock);
		#endif
	}
	if (syncGetTerminateFlag() != 0)
	{
		isInterrupted = true;

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "producer_decompress: interrupt2 - TerminateFlag set. "
			"Last produced=%d\n", lastBlock);
		#endif
	}

	if (isInterrupted)
	{
		close(hInfile);
		disposeMemorySingle(fileData);

		return 1;
	}

	return 0;
}

/*
 *********************************************************
    Function works in single pass. It's Splitting long
    streams into sequences of multiple segments.
 */
int producer_decompress(int hInfile, OFF_T fileSize, queue *fifo)
{
	safe_mutex_lock(&ProgressIndicatorsMutex);
	NumBlocks = 0;
	safe_mutex_unlock(&ProgressIndicatorsMutex);
	
	pbzip2::BZ2StreamScanner bz2StreamScanner(hInfile);
	
	// keep going until all the blocks are processed
	outBuff * fileData = bz2StreamScanner.getNextStream();
	while (!bz2StreamScanner.failed() && (fileData->bufSize > 0))
	{
		#ifdef PBZIP_DEBUG
		fprintf(stderr, " -> Bytes Read: %u bytes...\n", fileData->bufSize);
		#endif

		if (producerDecompressCheckInterrupt(hInfile, fileData, NumBlocks) != 0)
		{
			safe_mutex_lock(fifo->mut);
			safe_cond_broadcast(fifo->notEmpty); // just in case
			safe_mutex_unlock(fifo->mut);
			syncSetProducerDone(1);
			return 0;
		}

		if (QuietMode != 1)
		{
			// give warning to user if block is larger than 250 million bytes
			if (fileData->bufSize > 250000000)
			{
				fprintf(stderr, "pbz2:  *WARNING: Compressed block size is large [%"PRIuMAX" bytes].\n",
						(uintmax_t) fileData->bufSize);
				fprintf(stderr, "          If program aborts, use regular BZIP2 to decompress.\n");
			}
		}

		// add data to the decompression queue
		safe_mutex_lock(fifo->mut);
		while (fifo->full) 
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "producer: queue FULL.\n");
			#endif
			safe_cond_wait (fifo->notFull, fifo->mut);

			if (producerDecompressCheckInterrupt(hInfile, fileData, NumBlocks) != 0)
			{
				safe_cond_broadcast(fifo->notEmpty); // just in case
				syncSetProducerDone(1);
				safe_mutex_unlock(fifo->mut);
				return 0;
			}
		}
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "producer:  Buffer: %p  Size: %"PRIuMAX"   Block: %d\n", fileData->buf,
			(uintmax_t)fileData->bufSize, NumBlocks);
		#endif

		fifo->add(fileData);
		safe_cond_signal (fifo->notEmpty);

		safe_mutex_lock(&ProgressIndicatorsMutex);
		NumBlocks = fileData->blockNumber + 1;
		safe_mutex_unlock(&ProgressIndicatorsMutex);

		safe_mutex_unlock(fifo->mut);

		fileData = bz2StreamScanner.getNextStream();
	} // for

	close(hInfile);

	// last stream is always dummy one (either error or eof)
	delete fileData;


	if (bz2StreamScanner.failed())
	{
		handle_error(EF_EXIT, 1, "pbz2: producer_decompress: *ERROR: when reading bzip2 input stream\n");
		return -1;
	}
	else if (!bz2StreamScanner.isBz2HeaderFound() || !bz2StreamScanner.eof())
	{
		handle_error(EF_EXIT, 1, "pbz2: producer_decompress: *ERROR: input file is not a valid bzip2 stream\n");
		return -1;
	}

	syncSetProducerDone(1);
	safe_mutex_lock(fifo->mut);
	safe_cond_broadcast(fifo->notEmpty); // just in case
	safe_mutex_unlock(fifo->mut);

	#ifdef PBZIP_DEBUG
		fprintf(stderr, "producer:  Done - exiting. Last Block: %d\n", NumBlocks);
	#endif
	
	return 0;
}

/**
 * Check for interrupt conditions - report if any and perform the relevant
 * cleanup
 *
 * @return 0 - not interrupted; 1 - interrupted (terminate flag or other error encountered)
 */
int consumerDecompressCheckInterrupt(const outBuff * lastElement)
{
	bool isInterrupted = false;

	#ifdef PBZIP_DEBUG
	uintmax_t thid = (uintmax_t) pthread_self();
	#endif

	if (syncGetTerminateFlag() != 0)
	{
		isInterrupted = true;

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "(%"PRIuMAX") consumer_decompress: interrupt1 - TerminateFlag set.\n", thid);
		#endif
	}
	int minErrBlock = syncGetMinErrorBlock();
	if ( (minErrBlock != -1) &&
		( (lastElement == NULL)
			|| (lastElement->blockNumber >= minErrBlock)
			|| lastElement->isLastInSequence ) )
	{
		isInterrupted = true;

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "(%"PRIuMAX") consumer_decompress: terminating1 - LastGoodBlock set [%d].\n", thid, syncGetLastGoodBlock());
		#endif
	}

	if (isInterrupted)
	{
		return 1;
	}

	return 0;
}

/*
 *********************************************************
 */
void *consumer_decompress(void *q)
{
	queue *fifo = (queue *)q;
	
	outBuff *fileData = NULL;
	outBuff *lastFileData = NULL;
	char *DecompressedData = NULL;
	unsigned int outSize = 0;
	outBuff * prevOutBlockInSequence = NULL;
	int outSequenceNumber = -1; // sequence number in multi-part output blocks
	unsigned int processedIn = 0;
	int errState = 0;

	bz_stream strm;
	strm.bzalloc = NULL;
	strm.bzfree = NULL;
	strm.opaque = NULL;

	for (;;)
	{
		safe_mutex_lock(fifo->mut);
		for (;;)
		{
			if (consumerDecompressCheckInterrupt(fileData) != 0)
			{
				safe_mutex_unlock(fifo->mut);
				return (NULL);
			}
			
			if (!fifo->empty && (fifo->remove(fileData) == 1))
			{
				// block retreived - break the loop and continue further
				break;
			}

			#ifdef PBZIP_DEBUG
			fprintf (stderr, "consumer: queue EMPTY.\n");
			#endif
			
			if (fifo->empty && ((syncGetProducerDone() == 1) || (syncGetTerminateFlag() != 0)))
			{
				// finished - either OK or terminated forcibly
				pthread_mutex_unlock(fifo->mut);
				// BZ2_bzDecompressEnd( &strm );

				if ((syncGetTerminateFlag() == 0) && (outSequenceNumber != -1))
				{
					handle_error(EF_EXIT, -1, "pbz2: *ERROR on decompress - "
						"premature end of archive stream (block=%d; seq=%d; outseq=%d)!\n",
						lastFileData->blockNumber,
						lastFileData->sequenceNumber,
						outSequenceNumber);
				}
				#ifdef PBZIP_DEBUG
				else
				{
					fprintf (stderr, "consumer: exiting2\n");
				}
				#endif

				disposeMemorySingle( lastFileData );

				return (NULL);
			}

			#ifdef PBZIP_DEBUG
			safe_cond_timed_wait(fifo->notEmpty, fifo->mut, 1, "consumer");
			#else
			safe_cond_wait(fifo->notEmpty, fifo->mut);
			#endif
		}
		
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "consumer:  FileData: %p\n", fileData);
		fprintf(stderr, "consumer:  Buffer: %p  Size: %u   Block: %d\n",
				fileData->buf, (unsigned)fileData->bufSize, fileData->blockNumber);
		#endif

		safe_cond_signal(fifo->notFull);
		safe_mutex_unlock(fifo->mut);

		if (lastFileData != NULL)
		{
			delete lastFileData;
		}
		lastFileData = fileData;
		
		#ifdef PBZIP_DEBUG
		fprintf (stderr, "consumer: recieved %d.\n", fileData->blockNumber);
		#endif

		outSize = 900000;

		int bzret = BZ_OK;
		
		if (fileData->sequenceNumber < 2)
		{
			// start of new stream from in queue (0 -> single block; 1 - mutli)
			bzret = BZ2_bzDecompressInit(&strm, Verbosity, 0);
			if (bzret != BZ_OK)
			{
				handle_error(EF_EXIT, -1, "pbz2: *ERROR during BZ2_bzDecompressInit: %d\n", bzret);
				return (NULL);
			}
		}

		strm.avail_in = fileData->bufSize;
		strm.next_in = fileData->buf;
		while ((bzret == BZ_OK) && (strm.avail_in != 0))
		{
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "decompress: block=%d; seq=%d; prev=%llx; avail_in=%u; avail_out=%u\n",
				 fileData->blockNumber, outSequenceNumber,
				 (unsigned long long) prevOutBlockInSequence,
				 strm.avail_in, strm.avail_out);
			#endif

			if (DecompressedData == NULL)
			{
				// allocate memory for decompressed data (start with default 900k block size)
				DecompressedData = new(std::nothrow) char[outSize];
				// make sure memory was allocated properly
			
				if (DecompressedData == NULL)
				{
					handle_error(EF_EXIT, -1,
							" *ERROR: Could not allocate memory (DecompressedData)!  Aborting...\n");
					return (NULL);
				}

				processedIn = 0;

				strm.avail_out = outSize;
				strm.next_out = DecompressedData;
			}

			unsigned int availIn = strm.avail_in;
			bzret = BZ2_bzDecompress(&strm);
			processedIn += (availIn - strm.avail_in);

			#ifdef PBZIP_DEBUG
			fprintf(stderr, "decompress: BZ2_bzDecompress=%d; block=%d; seq=%d; prev=%llx; avail_in=%u; avail_out=%u\n",
				 bzret,
				 fileData->blockNumber, outSequenceNumber,
				 (unsigned long long) prevOutBlockInSequence,
				 strm.avail_in, strm.avail_out);
			#endif

			// issue out block if out buffer is full or stream end is detected
			if ( ((bzret == BZ_OK) && strm.avail_out == 0) || (bzret == BZ_STREAM_END) )
			{
				outBuff * addret = NULL;
				unsigned int len = outSize - strm.avail_out;
				bool isLast = (bzret == BZ_STREAM_END);
				
				if ( hasTrailingGarbage( bzret, fileData, strm ) )
				{
					// trailing garbage detected
					syncSetLastGoodBlock(fileData->blockNumber, fileData->blockNumber);
				}

				if (outSequenceNumber>0)
				{
					++outSequenceNumber;

					outBuff * nextOutBlock = new(std::nothrow) outBuff(
						DecompressedData, len, fileData->blockNumber,
						outSequenceNumber, processedIn, isLast, NULL);

					if (nextOutBlock == NULL)
					{
						BZ2_bzDecompressEnd( &strm );
						handle_error(EF_EXIT, -1,
								" *ERROR: Could not allocate memory (nextOutBlock)!  Aborting...\n");
						return (NULL);
					}

					addret = outputBufferSeqAddNext(prevOutBlockInSequence, nextOutBlock);
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "decompress: outputBufferSeqAddNext->%llx; block=%d; seq=%d; prev=%llx\n",
						(unsigned long long)addret,
						fileData->blockNumber, outSequenceNumber,
						(unsigned long long) prevOutBlockInSequence);
					#endif
				}
				else // sequenceNumber = 0
				{
					outSequenceNumber = (bzret == BZ_OK) ? 1 : 0;
					addret = outputBufferAdd(outBuff(
						DecompressedData, len,
						fileData->blockNumber,
						outSequenceNumber, processedIn, isLast, NULL), "consumer_decompress");

					#ifdef PBZIP_DEBUG
					fprintf(stderr, "decompress: outputBufferAdd->%llx; block=%d; seq=%d; prev=%llx\n",
						(unsigned long long)addret,
						fileData->blockNumber, outSequenceNumber,
						(unsigned long long) prevOutBlockInSequence);
					#endif
				}

				if (addret == NULL)
				{
					// error encountered
					BZ2_bzDecompressEnd( &strm );
					return (NULL);
				}

				prevOutBlockInSequence = addret;
				DecompressedData = NULL;
			}
		}

		/*
		 * < 0 - fatal error;
		 *   0 - OK (no error at all);
		 *   1 - first block of ignored trailing garbage;
		 *   2 - error already signalled for earlier block
		 */
		errState = decompressErrCheck(bzret, fileData, outSequenceNumber, strm);
		
		if (bzret == BZ_STREAM_END)
		{
			bzret = BZ2_bzDecompressEnd(&strm);
			if ( (bzret != BZ_OK) && ((errState == 0) || (errState == 1)) )
			{
				handle_error(EF_EXIT, -1, "pbz2: *ERROR during BZ2_bzDecompressEnd: %d\n", bzret);
				return (NULL);
			}

			outSequenceNumber = -1;
			prevOutBlockInSequence = NULL;
		}

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "\n Compressed Block Size: %u\n", (unsigned)fileData->bufSize);
		fprintf(stderr, "   Original Block Size: %u\n", outSize);
		#endif

		disposeMemory(fileData->buf);

		#ifdef PBZIP_DEBUG
		fprintf(stderr, " OutputBuffer[%d].buf = %p\n", fileData->blockNumber, DecompressedData);
		fprintf(stderr, " OutputBuffer[%d].bufSize = %u\n", fileData->blockNumber, outSize);
		fflush(stderr);
		#endif

		if (errState != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "consumer: exiting prematurely: errState=%d\n", errState);
			#endif

			return (NULL);
		}
	} // for
	
	#ifdef PBZIP_DEBUG
	fprintf (stderr, "consumer: exiting\n");
	#endif
	return (NULL);
}

/*
 *********************************************************
 */
void *fileWriter(void *outname)
{
	char *OutFilename;
	OFF_T CompressedSize = 0;
	int percentComplete = 0;
	int hOutfile = STDOUT_FILENO;  // default to stdout
	int currBlock = 0;
	size_t outBufferPos = 0;
	int ret = -1;
	OFF_T bytesProcessed = 0;

	OutFilename = (char *) outname;
	outBuff * prevBlockInSequence = NULL;

	#ifdef PBZIP_DEBUG
	fprintf(stderr, "fileWriter function started\n");
	#endif

	// write to file instead of stdout
	if (OutputStdOut == 0)
	{
		hOutfile = safe_open_output(OutFilename);
		// check to see if file creation was successful
		if (hOutfile == -1)
		{
			handle_error(EF_EXIT, -1,
				"pbz2: *ERROR: Could not create output file [%s]!\n", OutFilename);
			return (NULL);
		}
	}

	while (true)
	{
		#ifdef PBZIP_DEBUG
		int lastseq = 0;
		if (prevBlockInSequence != NULL)
		{
			lastseq = prevBlockInSequence->sequenceNumber;
		}
		#endif

		// Order is important. We don't need sync on NumBlocks when producer
		// is done.
		if ((syncGetProducerDone() == 1) && (currBlock >= NumBlocks) && (prevBlockInSequence == NULL))
		{
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "fileWriter [b:%d:%d]: done - quit loop.\n", currBlock, lastseq);
			#endif
			// We're done
			break;
		}

		if (syncGetTerminateFlag() != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "fileWriter [b:%d]: terminating1 - terminateFlag set\n", currBlock);
			#endif
			break;
		}

		safe_mutex_lock(OutMutex);
		#ifdef PBZIP_DEBUG
		outBuff * lastnext = (prevBlockInSequence != NULL) ? prevBlockInSequence->next : NULL;
		fprintf(stderr, "fileWriter:  Block: %d Size: %"PRIuMAX" Next File Block: %d"
				", outBufferPos: %"PRIuMAX", NumBlocks: %d, producerDone: %d, lastseq=%d"
				", prev=%p, next=%p\n",
				currBlock, (uintmax_t)NumBufferedBlocksMax, NextBlockToWrite,
				(uintmax_t)outBufferPos, NumBlocks, syncGetProducerDone(), lastseq,
				prevBlockInSequence,
				lastnext);
		#endif

		if ( (LastGoodBlock != -1) && (NextBlockToWrite > LastGoodBlock) )
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "fileWriter [b:%d]: quit - LastGoodBlock=%d\n",
					 currBlock, LastGoodBlock);
			#endif
			safe_mutex_unlock(OutMutex);
			
			break;
		}

		if ((OutputBuffer[outBufferPos].buf == NULL) &&
			((prevBlockInSequence == NULL) || (prevBlockInSequence->next == NULL)))
		{
			safe_cond_timed_wait(&OutBufferHeadNotEmpty, OutMutex, 1, "fileWriter");
			safe_mutex_unlock(OutMutex);
			// sleep a little so we don't go into a tight loop using up all the CPU
			// usleep(50000);
			continue;
		}
		else
		{
			safe_mutex_unlock(OutMutex);
		}

		outBuff * outBlock;
		if (prevBlockInSequence != NULL)
		{
			outBlock = prevBlockInSequence->next;
		}
		else
		{
			outBlock = &OutputBuffer[outBufferPos];
		}

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "fileWriter:  Buffer: %p  Size: %u   Block: %d, Seq: %d, isLast: %d\n",
			OutputBuffer[outBufferPos].buf, OutputBuffer[outBufferPos].bufSize, currBlock,
			outBlock->sequenceNumber, (int)outBlock->isLastInSequence);
		#endif

		// write data to the output file
		ret = do_write(hOutfile, outBlock->buf, outBlock->bufSize);

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "\n -> Total Bytes Written[%d:%d]: %d bytes...\n", currBlock, outBlock->sequenceNumber, ret);
		#endif

		if (ret < 0)
		{
			if (OutputStdOut == 0)
				close(hOutfile);

			handle_error(EF_EXIT, -1,
				"pbz2: *ERROR: Could not write %d bytes to file [ret=%d]!  Aborting...\n",
				outBlock->bufSize, ret);
			return (NULL);
		}
		CompressedSize += ret;
		
		bytesProcessed += outBlock->inSize;
		delete [] outBlock->buf;
		outBlock->buf = NULL;
		outBlock->bufSize = 0;

		if (outBlock->isLastInSequence)
		{
			if (++outBufferPos == NumBufferedBlocksMax)
			{
				outBufferPos = 0;
			}
			++currBlock;
		}

		safe_mutex_lock(OutMutex);

		if (outBlock->isLastInSequence)
		{
			++NextBlockToWrite;
			OutBufferPosToWrite = outBufferPos;
		}
		if (outBlock->sequenceNumber > 1)
		{
			--NumBufferedTailBlocks;
		}
		// --NumBufferedBlocks; // to be removed
		safe_cond_broadcast(notTooMuchNumBuffered);
		safe_cond_broadcast(&ErrStateChangeCond);
		safe_mutex_unlock(OutMutex);

		if (outBlock->sequenceNumber > 2)
		{
			delete prevBlockInSequence;
		}

		if (outBlock->isLastInSequence)
		{
			prevBlockInSequence = NULL;
			if (outBlock->sequenceNumber > 1)
			{
				delete outBlock;
			}
		}
		else
		{
			prevBlockInSequence = outBlock;
		}

		if (QuietMode != 1)
		{
			// print current completion status
			int percentCompleteOld = percentComplete;
			if (InFileSize > 0)
			{
				percentComplete = (100.0 * (double)bytesProcessed / (double)InFileSize);
			}
			
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "Completed: %d%%  NextBlockToWrite: %d/%"PRIuMAX"        \r", percentComplete, NextBlockToWrite, (uintmax_t)NumBufferedBlocksMax);
			fflush(stderr);
			#else
			if (percentComplete != percentCompleteOld)
			{
				fprintf(stderr, "Completed: %d%%             \r", percentComplete);
				fflush(stderr);
			}
			#endif
		}
	} // while

	if (currBlock == 0)
	{
		// zero-size file needs special handling
		ret = do_write(hOutfile, Bz2HeaderZero, sizeof(Bz2HeaderZero) );

		if (ret < 0)
		{
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not write to file!  Aborting...\n");
			return (NULL);
		}
	}


	if (OutputStdOut == 0)
	{
		ret = close(hOutfile);
		if (ret == -1)
		{
			ErrorContext::getInstance()->saveError();
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not close output file!  Aborting...\n");
			return (NULL);
		}
	}
	
	if (QuietMode != 1)
	{
		fprintf(stderr, "    Output Size: %"PRIuMAX" bytes\n", (uintmax_t)CompressedSize);
	}

	#ifdef PBZIP_DEBUG
	fprintf(stderr, "fileWriter exit\n");
	fflush(stderr);
	#endif

	// wake up all other possibly blocked on cond threads
	if (FifoQueue != NULL)
	{
		safe_mutex_lock(FifoQueue->mut);
		safe_cond_broadcast(FifoQueue->notEmpty); // important
		safe_cond_broadcast(FifoQueue->notFull); // not really needed
		safe_mutex_unlock(FifoQueue->mut);
	}
	safe_mutex_lock(OutMutex);
	safe_cond_broadcast(notTooMuchNumBuffered); // not really needed
	safe_mutex_unlock(OutMutex);

	if (QuietMode != 1)
	{
		// print current completion status
		percentComplete = 100;

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "Completed: %d%%  NextBlockToWrite: %d/%"PRIuMAX"        \r", percentComplete, NextBlockToWrite, (uintmax_t)NumBufferedBlocksMax);
		fflush(stderr);
		#else

			fprintf(stderr, "Completed: %d%%             \r", percentComplete);
			fflush(stderr);
		#endif
	}

	return (NULL);
}

/*
 *********************************************************
 */
int directcompress(int hInfile, OFF_T fileSize, int blockSize, const char *OutFilename)
{
	char *FileData = NULL;
	char *CompressedData = NULL;
	OFF_T CompressedSize = 0;
	OFF_T bytesLeft = 0;
	OFF_T inSize = 0;
	unsigned int outSize = 0;
	int percentComplete = 0;
	int hOutfile = STDOUT_FILENO;  // default to stdout
	int currBlock = 0;
	int rret = 0;
	int ret = 0;

	bytesLeft = fileSize;

	// write to file instead of stdout
	if (OutputStdOut == 0)
	{
		hOutfile = safe_open_output(OutFilename);
		// check to see if file creation was successful
		if (hOutfile == -1)
		{
			handle_error(EF_EXIT, -1, "pbz2 *ERROR: Could not create output file [%s]!\n", OutFilename);
			return -1;
		}
	}
    #ifdef WIN32        
	else
	{
        setmode(fileno(stdout), O_BINARY);
    }
    #endif

	// keep going until all the file is processed
	while (bytesLeft > 0)
	{
		if (syncGetTerminateFlag() != 0)
		{
			close(hInfile);
			if (OutputStdOut == 0)
				close(hOutfile);

			fprintf (stderr, "directcompress: terminating - terminateFlag set\n");

			return -1;
		}

		//
		// READ DATA
		//
		
		// set buffer size
		if (bytesLeft > blockSize)
			inSize = blockSize;
		else
			inSize = bytesLeft;

		#ifdef PBZIP_DEBUG
		fprintf(stderr, " -> Bytes To Read: %"PRIuMAX" bytes...\n", (uintmax_t)inSize);
		#endif

		// allocate memory to read in file
		FileData = NULL;
		FileData = new(std::nothrow) char[inSize];
		// make sure memory was allocated properly
		if (FileData == NULL)
		{
			close(hInfile);
			if (OutputStdOut == 0)
				close(hOutfile);

			handle_error(EF_EXIT, -1,
					 "pbz2 *ERROR: Could not allocate memory (FileData)!  Aborting...\n");
			return -1;
		}

		// read file data
		rret = do_read(hInfile, (char *) FileData, inSize);
		#ifdef PBZIP_DEBUG
		fprintf(stderr, " -> Total Bytes Read: %d bytes...\n\n", rret);
		#endif
		if (rret == 0)
		{
			if (FileData != NULL)
				delete [] FileData;
			break;
		}
		else if (rret < 0)
		{
			close(hInfile);
			if (FileData != NULL)
				delete [] FileData;
			if (OutputStdOut == 0)
				close(hOutfile);

			handle_error(EF_EXIT, -1,
					"pbz2: *ERROR: Could not read from file!  Aborting...\n");
			return -1;
		}

		// set bytes left after read
		bytesLeft -= rret;

		//
		// COMPRESS DATA
		//
			
		outSize = (int) ((inSize*1.01)+600);
		// allocate memory for compressed data
		CompressedData = new(std::nothrow) char[outSize];
		// make sure memory was allocated properly
		if (CompressedData == NULL)
		{
			close(hInfile);
			if (FileData != NULL)
				delete [] FileData;

			handle_error(EF_EXIT, -1,
					"pbz2: *ERROR: Could not allocate memory (CompressedData)!  Aborting...\n");
			return -1;
		}

		// compress the memory buffer (blocksize=9*100k, verbose=0, worklevel=30)
		ret = BZ2_bzBuffToBuffCompress(CompressedData, &outSize, FileData, inSize, BWTblockSize, Verbosity, 30);
		if (ret != BZ_OK)
		{
			close(hInfile);
			if (FileData != NULL)
				delete [] FileData;

			handle_error(EF_EXIT, -1, "pbz2: *ERROR during compression: %d!  Aborting...\n", ret);
			return -1;
		}

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "\n   Original Block Size: %"PRIuMAX"\n", (uintmax_t)inSize);
		fprintf(stderr, " Compressed Block Size: %u\n", outSize);
		#endif

		//
		// WRITE DATA
		//

		// write data to the output file
		ret = do_write(hOutfile, CompressedData, outSize);

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "\n -> Total Bytes Written[%d]: %d bytes...\n", currBlock, ret);
		#endif
		if (ret <= 0)
		{
			close(hInfile);
			if (FileData != NULL)
				delete [] FileData;
			if (CompressedData != NULL)
				delete [] CompressedData;
			if (OutputStdOut == 0)
				close(hOutfile);

			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not write to file!  Aborting...\n");
			return -1;
		}

		CompressedSize += ret;

		currBlock++;
		// print current completion status
		int percentCompleteOld = percentComplete;
		percentComplete = 100 * currBlock / NumBlocksEstimated;
		if (QuietMode != 1)
		{
			if (percentComplete != percentCompleteOld)
			{
				fprintf(stderr, "Completed: %d%%             \r", percentComplete);
				fflush(stderr);
			}
		}
		
		// clean up memory
		if (FileData != NULL)
		{
			delete [] FileData;
			FileData = NULL;
		}
		if (CompressedData != NULL)
		{
			delete [] CompressedData;
			CompressedData = NULL;
		}
		
		// check to make sure all the data we expected was read in
		if (rret != inSize)
			inSize = rret;
	} // while

	close(hInfile);
	
	if (OutputStdOut == 0)
		close(hOutfile);
	if (QuietMode != 1)
	{
		fprintf(stderr, "    Output Size: %"PRIuMAX" bytes\n", (uintmax_t)CompressedSize);
	}

	syncSetProducerDone(1); // Not really needed for direct version
	return 0;
}

void close_streams(FILE *out, FILE *in)
{
	if (out != NULL) {
		fflush(out);
	}

	if (in != NULL && in != stdin) {
		fclose(in);
	}

	if (out != NULL && out != stdout) {
		fclose(out);
	}
}

/*
 *********************************************************
 */
int directdecompress(const char *InFilename, const char *OutFilename)
{
	FILE *stream = NULL;
	FILE *zStream = NULL;
	BZFILE* bzf = NULL;
	unsigned char obuf[5000];
	unsigned char unused[BZ_MAX_UNUSED];
	unsigned char *unusedTmp;
	int bzerr, nread, streamNo;
	int nUnused;
	int ret = 0;
	int i;

	nUnused = 0;
	streamNo = 0;

	// see if we are using stdin or not
	if (strcmp(InFilename, "-") != 0) 
	{
		// open the file for reading
		zStream = fopen(InFilename, "rb");
		if (zStream == NULL)
		{
			handle_error(EF_EXIT, -1,
					"pbz2: *ERROR: Could not open input file [%s]!  Aborting...\n", InFilename);
			return -1;
		}
	}
	else
	{
		#ifdef WIN32        
        setmode(fileno(stdin), O_BINARY);
		#endif
		zStream = stdin;
    }

	// check file stream for errors
	if (ferror(zStream))
	{
		close_streams(stream, zStream);
		handle_error(EF_EXIT, -1,
				"pbz2: *ERROR: Problem with input stream of file [%s]!  Aborting...\n", InFilename);
		return -1;
	}

	// see if we are outputting to stdout
	if (OutputStdOut == 0)
	{
		stream = safe_fopen_output(OutFilename, "wb");
		if (stream == NULL)
		{
			handle_error(EF_EXIT, -1,
					"pbz2: *ERROR: Could not open output file [%s]!  Aborting...\n", OutFilename);
			return -1;
		}
	}
	else
	{
        #ifdef WIN32        
        setmode(fileno(stdout), O_BINARY);
        #endif
		stream = stdout;
    }

	// check file stream for errors
	if (ferror(stream))
	{
		close_streams(stream, zStream);
		handle_error(EF_EXIT, -1,
				"pbz2: *ERROR: Problem with output stream of file [%s]!  Aborting...\n", InFilename);
		return -1;
	}

	// loop until end of file
	while(true)
	{
		if (syncGetTerminateFlag() != 0)
		{
			fprintf (stderr, "directdecompress: terminating1 - terminateFlag set\n");
			close_streams(stream, zStream);
			return -1;
		}

		bzf = BZ2_bzReadOpen(&bzerr, zStream, Verbosity, 0, unused, nUnused);
		if (bzf == NULL || bzerr != BZ_OK)
		{
			ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
			close_streams(stream, zStream);
	
			if (ret != 0)
			{
				syncSetTerminateFlag(1);
			}

			return ret;
		}

		streamNo++;
		
		while (bzerr == BZ_OK)
		{
			if (syncGetTerminateFlag() != 0)
			{
				fprintf (stderr, "directdecompress: terminating2 - terminateFlag set\n");
				close_streams(stream, zStream);
				return -1;
			}

			nread = BZ2_bzRead(&bzerr, bzf, obuf, sizeof(obuf));
			if (bzerr == BZ_DATA_ERROR_MAGIC)
			{
				// try alternate way of reading data
				if (ForceOverwrite == 1)
				{
					rewind(zStream);
					while (true)
					{
						int c = fgetc(zStream);
						if (c == EOF)
							break;
						ungetc(c,zStream);
					 
						nread = fread(obuf, sizeof(unsigned char), sizeof(obuf), zStream );
						if (ferror(zStream))
						{
							ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
							close_streams(stream, zStream);

							if (ret != 0)
							{
								syncSetTerminateFlag(1);
							}
							
							return ret;
						}
						if (nread > 0)
							(void) fwrite (obuf, sizeof(unsigned char), nread, stream);
						if (ferror(stream))
						{
							ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
							close_streams(stream, zStream);

							if (ret != 0)
							{
								syncSetTerminateFlag(1);
							}
							
							return ret;
						}
					}
					goto closeok;
				}
			}
			if ((bzerr == BZ_OK || bzerr == BZ_STREAM_END) && nread > 0)
				(void) fwrite(obuf, sizeof(unsigned char), nread, stream );
			if (ferror(stream))
			{
				ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
				close_streams(stream, zStream);

				if (ret != 0)
				{
					syncSetTerminateFlag(1);
				}
				return ret;
			}
		}
		if (bzerr != BZ_STREAM_END)
		{
			ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
			close_streams(stream, zStream);

			if (ret != 0)
			{
				syncSetTerminateFlag(1);
			}
			return ret;
		}

		BZ2_bzReadGetUnused(&bzerr, bzf, (void**)(&unusedTmp), &nUnused);
		if (bzerr != BZ_OK)
		{
			handle_error(EF_EXIT, 3, "pbz2: *ERROR: Unexpected error [bzerr=%d]. Aborting!\n", bzerr);
			return 3;
		}

		for (i = 0; i < nUnused; i++)
			unused[i] = unusedTmp[i];

		BZ2_bzReadClose(&bzerr, bzf);
		if (bzerr != BZ_OK)
		{
			handle_error(EF_EXIT, 3, "pbz2: *ERROR: Unexpected error [bzerr=%d]. Aborting!\n", bzerr);
			return 3;
		}

		// check to see if we are at the end of the file
		if (nUnused == 0)
		{
			int c = fgetc(zStream);
			if (c == EOF)
				break;
			ungetc(c, zStream);
		}
	}
	
closeok:
	// check file stream for errors
	if (ferror(zStream))
	{
		if (zStream != stdin)
			fclose(zStream);
		if (stream != stdout)
			fclose(stream);

		handle_error(EF_EXIT, -1, "pbz2: *ERROR: Problem with input stream of file [%s]!  Skipping...\n", InFilename);
		
		return -1;
	}
	// close file
	ret = do_fclose(zStream);
	if (ret == EOF)
	{
		handle_error(EF_EXIT, -1, "pbz2: *ERROR: Problem closing file [%s]!  Skipping...\n", InFilename);
		return -1;
	}

	// check file stream for errors
	if (ferror(stream))
	{
		if (stream != stdout)
			fclose(stream);
		handle_error(EF_EXIT, -1, "pbz2: *ERROR: Problem with output stream of file [%s]!  Skipping...\n", InFilename);
		
		return -1;
	}
	ret = do_fflush(stream);
	if (ret != 0)
	{
		if (stream != stdout)
			fclose(stream);
		handle_error(EF_EXIT, -1, "pbz2: *ERROR: Problem with output stream of file [%s]!  Skipping...\n", InFilename);
		return -1;
	}
	if (stream != stdout)
	{
		ret = do_fclose(stream);
		if (ret == EOF)
		{
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Problem closing file [%s]!  Skipping...\n", OutFilename);
			return -1;
		}
	}

	syncSetProducerDone(1); // Not really needed for direct version.
	return 0;
}

/*
 * Simulate an unconditional read(), reading in data to fill the
 * bsize-sized buffer if it can, even if it means calling read() multiple
 * times. This is needed since pipes and other "special" streams
 * sometimes don't allow reading of arbitrary sized buffers.
 */
ssize_t bufread(int hf, char *buf, size_t bsize)
{
	size_t bufr = 0;
	int ret;
	int rsize = bsize;

	while (1)
	{
		ret = read(hf, buf, rsize);

		if (ret < 0)
			return ret;
		if (ret == 0)
			return bufr;

		bufr += ret;
		if (bufr == bsize)
			return bsize;
		rsize -= ret;
		buf += ret;
	}
}

/*
 *********************************************************
 */
int producer(int hInfile, int blockSize, queue *fifo)
{
	char *FileData = NULL;
	size_t inSize = 0;
	// int blockNum = 0;
	int ret = 0;
	// int pret = -1;

	// We will now totally ignore the fileSize and read the data as it
	// comes in. Aside from allowing us to process arbitrary streams, it's
	// also the *right thing to do* in unix environments where data may
	// be appended to the file as it's processed (e.g. log files).

	safe_mutex_lock(&ProgressIndicatorsMutex);
	NumBlocks = 0;
	safe_mutex_unlock(&ProgressIndicatorsMutex);

	// keep going until all the file is processed
	while (1)
	{
		if (syncGetTerminateFlag() != 0)
		{
			close(hInfile);
			return -1;
		}

		// set buffer size
		inSize = blockSize;

		#ifdef PBZIP_DEBUG
		fprintf(stderr, " -> Bytes To Read: %"PRIuMAX" bytes...\n", (uintmax_t)inSize);
		#endif

		// allocate memory to read in file
		FileData = NULL;
		FileData = new(std::nothrow) char[inSize];
		// make sure memory was allocated properly
		if (FileData == NULL)
		{
			close(hInfile);
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not allocate memory (FileData)!  Aborting...\n");
			return -1;
		}

		// read file data
		ret = bufread(hInfile, (char *) FileData, inSize);
		#ifdef PBZIP_DEBUG
		fprintf(stderr, " -> Total Bytes Read: %d bytes...\n\n", ret);
		#endif
		if (ret == 0)
		{
			// finished reading.
			if (FileData != NULL)
				delete [] FileData;
			break;
		}
		else if (ret < 0)
		{
			close(hInfile);
			if (FileData != NULL)
				delete [] FileData;

			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not read from file!  Aborting...\n");
			return -1;
		}

		// check to make sure all the data we expected was read in
		if ((size_t)ret != inSize)
			inSize = ret;

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "producer:  Going into fifo-mut lock (NumBlocks: %d)\n", NumBlocks);
		#endif

		// add data to the compression queue
		safe_mutex_lock(fifo->mut);
		while (fifo->full)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "producer: queue FULL.\n");
			#endif
			safe_cond_wait(fifo->notFull, fifo->mut);

			if (syncGetTerminateFlag() != 0)
			{
				pthread_mutex_unlock(fifo->mut);
				close(hInfile);
				return -1;
			}
		}
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "producer:  Buffer: %p  Size: %"PRIuMAX"   Block: %d\n", FileData, (uintmax_t)inSize, NumBlocks);
		#endif

		outBuff * queueElement = new(std::nothrow) outBuff(FileData, inSize, NumBlocks, 0);
		// make sure memory was allocated properly
		if (queueElement == NULL)
		{
			close(hInfile);
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not allocate memory (queueElement)!  Aborting...\n");
			return -1;
		}

		fifo->add(queueElement);
		safe_cond_signal(fifo->notEmpty);

		safe_mutex_lock(&ProgressIndicatorsMutex);
		++NumBlocks;
		safe_mutex_unlock(&ProgressIndicatorsMutex);
		
		safe_mutex_unlock(fifo->mut);
	} // while

	close(hInfile);

	syncSetProducerDone(1);
	safe_mutex_lock(fifo->mut);
	safe_cond_broadcast(fifo->notEmpty); // just in case
	safe_mutex_unlock(fifo->mut);

	#ifdef PBZIP_DEBUG
		fprintf(stderr, "producer:  Done - exiting. Num Blocks: %d\n", NumBlocks);
	#endif

	return 0;
}

/*
 *********************************************************
 */
void *consumer (void *q)
{
	queue *fifo;
	// char *FileData = NULL;
	outBuff *fileData = NULL;
	char *CompressedData = NULL;
	// unsigned int inSize = 0;
	unsigned int outSize = 0;
	// int blockNum = -1;
	int ret = -1;

	fifo = (queue *)q;

	for (;;)
	{
		if (syncGetTerminateFlag() != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "consumer: terminating1 - terminateFlag set\n");
			#endif
			return (NULL);
		}

		safe_mutex_lock(fifo->mut);
		for (;;)
		{
			if (!fifo->empty && (fifo->remove(fileData) == 1))
			{
				// block retreived - break the loop and continue further
				break;
			}
			
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "consumer: queue EMPTY.\n");
			#endif
			
			if (fifo->empty && ((syncGetProducerDone() == 1) || (syncGetTerminateFlag() != 0)))
			{
				safe_mutex_unlock(fifo->mut);
				#ifdef PBZIP_DEBUG
				fprintf (stderr, "consumer: exiting2\n");
				#endif
				return (NULL);
			}

			#ifdef PBZIP_DEBUG
			safe_cond_timed_wait(fifo->notEmpty, fifo->mut, 1, "consumer");
			#else
			safe_cond_wait(fifo->notEmpty, fifo->mut);
			#endif
		}

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "consumer:  Buffer: %p  Size: %u   Block: %d\n",
				fileData->buf, (unsigned)fileData->bufSize, fileData->blockNumber);
		#endif

		safe_cond_signal(fifo->notFull);
		safe_mutex_unlock(fifo->mut);
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "consumer: received %d.\n", fileData->blockNumber);
		#endif

		outSize = (unsigned int) (((fileData->bufSize)*1.01)+600);
		// allocate memory for compressed data
		CompressedData = new(std::nothrow) char[outSize];
		// make sure memory was allocated properly
		if (CompressedData == NULL)
		{
			handle_error(EF_EXIT, -1, "pbz2: *ERROR: Could not allocate memory (CompressedData)!  Aborting...\n");
			return (NULL);
		}

		// compress the memory buffer (blocksize=9*100k, verbose=0, worklevel=30)
		ret = BZ2_bzBuffToBuffCompress(CompressedData, &outSize,
				fileData->buf, fileData->bufSize, BWTblockSize, Verbosity, 30);
		if (ret != BZ_OK)
		{
			handle_error(EF_EXIT, -1, "pbz2: *ERROR during compression: %d!  Aborting...\n", ret);
			return (NULL);
		}

		#ifdef PBZIP_DEBUG
		fprintf(stderr, "\n   Original Block Size: %u\n", (unsigned)fileData->bufSize);
		fprintf(stderr, " Compressed Block Size: %u\n", outSize);
		#endif

		disposeMemory(fileData->buf);

		// store data to be written in output bin
		outBuff outBlock = outBuff(CompressedData, outSize, fileData->blockNumber, 0, fileData->bufSize);
		if (outputBufferAdd(outBlock, "consumer") == NULL)
		{
			return (NULL);
		}

		delete fileData;
		fileData = NULL;
	} // for
	
	#ifdef PBZIP_DEBUG
	fprintf (stderr, "consumer: exiting\n");
	#endif
	return (NULL);
}

/*
 *********************************************************
 */
int mutexesInit()
{
	// initialize mutexes
	OutMutex = new(std::nothrow) pthread_mutex_t;
	// make sure memory was allocated properly
	if (OutMutex == NULL)
	{
		fprintf(stderr, "pbz2: *ERROR: Could not allocate memory (OutMutex)!  Aborting...\n");
		return 1;
	}
	pthread_mutex_init(OutMutex, NULL);

	ProducerDoneMutex = new(std::nothrow) pthread_mutex_t;
	// make sure memory was allocated properly
	if (ProducerDoneMutex == NULL)
	{
		fprintf(stderr, "pbz2: *ERROR: Could not allocate memory (ProducerDoneMutex)!  Aborting...\n");
		return 1;
	}
	pthread_mutex_init(ProducerDoneMutex, NULL);

	return 0;
}

/*
 *********************************************************
 */
void mutexesDelete()
{
	if (OutMutex != NULL)
	{
		pthread_mutex_destroy(OutMutex);
		delete OutMutex;
		OutMutex = NULL;
	}

	if (ProducerDoneMutex != NULL)
	{
		pthread_mutex_destroy(ProducerDoneMutex);
		delete ProducerDoneMutex;
		ProducerDoneMutex = NULL;
	}
}

/*
 *********************************************************
 */
queue *queueInit(int queueSize)
{
	queue *q;

	q = new(std::nothrow) queue;
	if (q == NULL)
		return NULL;

	q->qData = new(std::nothrow) queue::ElementTypePtr[queueSize];

	if (q->qData == NULL)
		return NULL;

	q->size = queueSize;

	q->clear();

	q->mut = NULL;
	q->mut = new(std::nothrow) pthread_mutex_t;
	if (q->mut == NULL)
		return NULL;
	pthread_mutex_init(q->mut, NULL);

	q->notFull = NULL;
	q->notFull = new(std::nothrow) pthread_cond_t;
	if (q->notFull == NULL)
		return NULL;
	pthread_cond_init(q->notFull, NULL);

	q->notEmpty = NULL;
	q->notEmpty = new(std::nothrow) pthread_cond_t;
	if (q->notEmpty == NULL)
		return NULL;
	pthread_cond_init(q->notEmpty, NULL);
	
	q->consumers = NULL;
	q->consumers = new(std::nothrow) pthread_t[queueSize];
	if (q->consumers == NULL)
		return NULL;

	notTooMuchNumBuffered = NULL;
	notTooMuchNumBuffered = new(std::nothrow) pthread_cond_t;
	if (notTooMuchNumBuffered == NULL)
		return NULL;
	pthread_cond_init(notTooMuchNumBuffered, NULL);

	return (q);
}


/*
 *********************************************************
 */
void queueDelete (queue *q)
{
	if (q == NULL)
		return;

	if (q->mut != NULL)
	{
		pthread_mutex_destroy(q->mut);
		delete q->mut;
		q->mut = NULL;
	}

	if (q->notFull != NULL)
	{
		pthread_cond_destroy(q->notFull);
		delete q->notFull;
		q->notFull = NULL;
	}

	if (q->notEmpty != NULL)
	{
		pthread_cond_destroy(q->notEmpty);
		delete q->notEmpty;
		q->notEmpty = NULL;
	}

    delete [] q->consumers;
	delete [] q->qData;

	delete q;
	q = NULL;

	if (notTooMuchNumBuffered != NULL)
	{
		pthread_cond_destroy(notTooMuchNumBuffered);
		delete notTooMuchNumBuffered;
		notTooMuchNumBuffered = NULL;
	}

	return;
}


/**
 * Initialize output buffer contents with empty (NULL, 0) blocks
 *
 * @param size new size of buffer
 *
 */
void outputBufferInit(size_t size)
{
	safe_mutex_lock(OutMutex);

	NextBlockToWrite = 0;
	OutBufferPosToWrite = 0;
	NumBufferedBlocks = 0;
	NumBufferedTailBlocks = 0;

	outBuff emptyElement;
	emptyElement.buf = NULL;
	emptyElement.bufSize = 0;

	// Resize and fill-in with empty elements
	OutputBuffer.assign(size, emptyElement);

	// unlikely to get here since more likely exception will be thrown
	if (OutputBuffer.size() != size)
	{
		fprintf(stderr, "pbz2: *ERROR: Could not initialize (OutputBuffer); size=%"PRIuMAX"!  Aborting...\n", (uintmax_t)size);
		safe_mutex_unlock(OutMutex);
		exit(1);
	}

	safe_mutex_unlock(OutMutex);
}

/**
 * Get output buffer index corresponding to the given absolute blockNumber
 * (buffer is used in circular mode)
 *
 * @param blockNum - absolute block number to translate
 * @return 0-based Output Buffer index where blockNum data should go
 */
inline size_t getOutputBufferPos(int blockNum)
{
	// calculate output buffer position (used in circular mode)
	size_t outBuffPos = OutBufferPosToWrite + blockNum - NextBlockToWrite;

	if (outBuffPos >= NumBufferedBlocksMax)
	{
		outBuffPos -= NumBufferedBlocksMax;
	}

	return outBuffPos;
}

/**
 * Add next element to the given out buffer tail.
 *
 */
outBuff * outputBufferSeqAddNext(outBuff * preveElement, outBuff * newElement)
{
	safe_mutex_lock(OutMutex);

	while ((NumBufferedTailBlocks >= NumBufferedBlocksMax) &&
			(preveElement->buf != NULL))
	{
		if (syncGetTerminateFlag() != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "%s: terminating2 - terminateFlag set\n", "consumer");
			#endif
			pthread_mutex_unlock(OutMutex);
			return NULL;
		}

		if ( (LastGoodBlock != -1) && (LastGoodBlock < newElement->blockNumber) )
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "%s: terminating3 - LastGoodBlock set\n", "consumer");
			#endif
			pthread_mutex_unlock(OutMutex);
			return NULL;
		}

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "%s/outputBufferSeqAddNext: Throttling from FileWriter backlog: %d\n", "consumer", NumBufferedBlocks);
		#endif
		safe_cond_wait(notTooMuchNumBuffered, OutMutex);
	}

	preveElement->next = newElement;

	++NumBufferedTailBlocks;

	// size_t outBufPos = getOutputBufferPos(newElement->blockNumber);
	if (preveElement->buf == NULL)
	{
		// fileWriter has already consumed the previous block. Let it know
		// for that one early
		safe_cond_signal(&OutBufferHeadNotEmpty);
	}

	safe_mutex_unlock(OutMutex);

	return newElement;
}

/**
 * Store an item in OutputBuffer out bin. Synchronization is embedded to protect
 * from simultaneous access.
 *
 * @param elem - output buffer element to add
 * @param caller - used for debug purposes (caller function name)
 *
 * @return pointer to added element on success; NULL - on error
 */
outBuff * outputBufferAdd(const outBuff & element, const char *caller)
{
	safe_mutex_lock(OutMutex);

	// wait while blockNum is out of range
	// [NextBlockToWrite, NextBlockToWrite + NumBufferedBlocksMax)
	int dist = element.blockNumber - NumBufferedBlocksMax;
	while (dist >= NextBlockToWrite)
	{
		if (syncGetTerminateFlag() != 0)
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "%s/outputBufferAdd: terminating2 - terminateFlag set\n", caller);
			#endif
			pthread_mutex_unlock(OutMutex);
			return NULL;
		}

		if ( (LastGoodBlock != -1) && (LastGoodBlock < element.blockNumber) )
		{
			#ifdef PBZIP_DEBUG
			fprintf (stderr, "%s: terminating3 - LastGoodBlock set\n", "consumer");
			#endif
			pthread_mutex_unlock(OutMutex);
			return NULL;
		}

		#ifdef PBZIP_DEBUG
		fprintf (stderr, "%s: Throttling from FileWriter backlog: %d\n", caller, NumBufferedBlocks);
		#endif
		safe_cond_wait(notTooMuchNumBuffered, OutMutex);
	}

	// calculate output buffer position (used in circular mode)
	size_t outBuffPos = getOutputBufferPos(element.blockNumber);

	OutputBuffer[outBuffPos] = element;
	++NumBufferedBlocks;

	if (NextBlockToWrite == element.blockNumber)
	{
		safe_cond_signal(&OutBufferHeadNotEmpty);
	}

	safe_mutex_unlock(OutMutex);

	return &(OutputBuffer[outBuffPos]);
}

/*
 *********************************************************
 Much of the code in this function is taken from bzip2.c
 */
int testBZ2ErrorHandling(int bzerr, BZFILE* bzf, int streamNo)
{
	int bzerr_dummy;

	BZ2_bzReadClose(&bzerr_dummy, bzf);
	switch (bzerr)
	{
		case BZ_CONFIG_ERROR:
			fprintf(stderr, "pbz2: *ERROR: Integers are not the right size for libbzip2. Aborting!\n");
			exit(3);
			break;
		case BZ_IO_ERROR:
			fprintf(stderr, "pbz2: *ERROR: Integers are not the right size for libbzip2. Aborting!\n");
			return 1;
			break;
		case BZ_DATA_ERROR:
			fprintf(stderr, "pbz2: *ERROR: Data integrity (CRC) error in data!  Skipping...\n");
			return -1;
			break;
		case BZ_MEM_ERROR:
			fprintf(stderr, "pbz2: *ERROR: Could NOT allocate enough memory. Aborting!\n");
			return 1;
			break;
		case BZ_UNEXPECTED_EOF:
			fprintf(stderr, "pbz2: *ERROR: File ends unexpectedly!  Skipping...\n");
			return -1;
			break;
		case BZ_DATA_ERROR_MAGIC:
			if (streamNo == 1)
			{
				fprintf(stderr, "pbz2: *ERROR: Bad magic number (file not created by bzip2)!  Skipping...\n");
				return -1;
			}
			else
			{
				fprintf(stderr, "pbz2: *WARNING: Trailing garbage after EOF ignored!\n");
				return 0;
			}
		default:
			fprintf(stderr, "pbz2: *ERROR: Unexpected error. Aborting!\n");
			exit(3);
	}

	return 0;
}

/*
 *********************************************************
 Much of the code in this function is taken from bzip2.c
 */
int testCompressedData(char *fileName)
{
	FILE *zStream = NULL;
	int ret = 0;

	BZFILE* bzf = NULL;
	unsigned char obuf[5000];
	unsigned char unused[BZ_MAX_UNUSED];
	unsigned char *unusedTmp;
	int bzerr, nread, streamNo;
	int nUnused;
	int i;

	nUnused = 0;
	streamNo = 0;

	// see if we are using stdin or not
	if (strcmp(fileName, "-") != 0) 
	{
		// open the file for reading
		zStream = fopen(fileName, "rb");
		if (zStream == NULL)
		{
			ErrorContext::getInstance()->saveError();
			handle_error(EF_NOQUIT, -1, "pbz2: *ERROR: Could not open input file [%s]!  Skipping...\n", fileName);
			return -1;
		}
	}
	else
	{
		zStream = stdin;
	}

	// check file stream for errors
	if (ferror(zStream))
	{
		
		handle_error(EF_NOQUIT, -1, "pbz2: *ERROR: Problem with stream of file [%s]!  Skipping...\n", fileName);
		if (zStream != stdin)
			verbose_fclose(zStream, fileName);
		return -1;
	}

	// loop until end of file
	while(true)
	{
		bzf = BZ2_bzReadOpen(&bzerr, zStream, Verbosity, 0, unused, nUnused);
		if (bzf == NULL || bzerr != BZ_OK)
		{
			ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
			if (zStream != stdin)
				verbose_fclose(zStream, fileName);
			return ret;
		}

		streamNo++;

		while (bzerr == BZ_OK)
		{
			nread = BZ2_bzRead(&bzerr, bzf, obuf, sizeof(obuf));
			if (bzerr == BZ_DATA_ERROR_MAGIC)
			{
				ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
				if (zStream != stdin)
					verbose_fclose(zStream, fileName);
				return ret;
			}
		}
		if (bzerr != BZ_STREAM_END)
		{
			ret = testBZ2ErrorHandling(bzerr, bzf, streamNo);
			if (zStream != stdin)
				verbose_fclose(zStream, fileName);
			return ret;
		}

		BZ2_bzReadGetUnused(&bzerr, bzf, (void**)(&unusedTmp), &nUnused);
		if (bzerr != BZ_OK)
		{
			fprintf(stderr, "pbz2: *ERROR: Unexpected error. Aborting!\n");
			exit(3);
		}

		for (i = 0; i < nUnused; i++)
			unused[i] = unusedTmp[i];

		BZ2_bzReadClose(&bzerr, bzf);
		if (bzerr != BZ_OK)
		{
			fprintf(stderr, "pbz2: *ERROR: Unexpected error. Aborting!\n");
			exit(3);
		}

		// check to see if we are at the end of the file
		if (nUnused == 0)
		{
			int c = fgetc(zStream);
			if (c == EOF)
				break;
			else
				ungetc(c, zStream);
		}
	}

	// check file stream for errors
	if (ferror(zStream))
	{
		ErrorContext::getInstance()->saveError();
		handle_error(EF_NOQUIT, -1, "pbz2: *ERROR: Problem with stream of file [%s]!  Skipping...\n", fileName);
		if (zStream != stdin)
			verbose_fclose(zStream, fileName);
		return -1;
	}

	// close file
	ret = verbose_fclose(zStream, fileName);
	if (ret == EOF)
	{
		fprintf(stderr, "pbz2: *ERROR: Problem closing file [%s]!  Skipping...\n", fileName);
		return -1;
	}

	return 0;
}

/*
 *********************************************************
 */
int getFileMetaData(const char *fileName)
{
	// get the file meta data and store it in the global structure
	return stat(fileName, &fileMetaData);
}

/*
 *********************************************************
 */
int writeFileMetaData(const char *fileName)
{
	int ret = 0;
	#ifndef WIN32
	struct utimbuf uTimBuf;
    #else
	_utimbuf uTimBuf;
    #endif

	// store file times in structure
	uTimBuf.actime = fileMetaData.st_atime;
	uTimBuf.modtime = fileMetaData.st_mtime;

	// update file with stored file permissions
	ret = chmod(fileName, fileMetaData.st_mode);
	if (ret != 0)
	{
		ErrorContext::getInstance()->saveError();
		return ret;
	}

	// update file with stored file access and modification times
	ret = utime(fileName, &uTimBuf);
	if (ret != 0)
	{
		ErrorContext::getInstance()->saveError();
		return ret;
	}

	// update file with stored file ownership (if access allows)
	#ifndef WIN32
	ret = chown(fileName, fileMetaData.st_uid, fileMetaData.st_gid);
	// following may happen on some Linux filesystems (i.e. NTFS)
	// extra error messages do no harm
	if (ret != 0)
	{
		ErrorContext::getInstance()->saveError();
		if (geteuid() == 0)
			return ret;
	}
	#endif

	return 0;
}

/*
 *********************************************************
 */
int detectCPUs()
{
	int ncpu;
	
	// Set default to 1 in case there is no auto-detect
	ncpu = 1;

	// Autodetect the number of CPUs on a box, if available
	#if defined(__APPLE__)
		size_t len = sizeof(ncpu);
		int mib[2];
		mib[0] = CTL_HW;
		mib[1] = HW_NCPU;
		if (sysctl(mib, 2, &ncpu, &len, 0, 0) < 0 || len != sizeof(ncpu))
			ncpu = 1;
	#elif defined(_SC_NPROCESSORS_ONLN)
		ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	#elif defined(WIN32)
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		ncpu = si.dwNumberOfProcessors;
	#endif

	// Ensure we have at least one processor to use
	if (ncpu < 1)
		ncpu = 1;

	return ncpu;
}


/*
 *********************************************************
 */
void banner()
{
    /*
	 *fprintf(stderr, "Parallel BZIP2 v1.1.13 [Dec 18, 2015]\n");
	 *fprintf(stderr, "By: Jeff Gilchrist [http://compression.ca]\n");
	 *fprintf(stderr, "Major contributions: Yavor Nikolov [http://javornikolov.wordpress.com]\n");
	 *fprintf(stderr, "Uses libbzip2 by Julian Seward\n");
     */

	return;
}

/*
 *********************************************************
 */
void usage(char* progname, const char *reason)
{
    /*
	 *banner();
     */
	
	if (strncmp(reason, "HELP", 4) == 0)
		fprintf(stderr, "\n");
	else
		fprintf(stderr, "\nInvalid command line: %s.  Aborting...\n\n", reason);

#ifndef PBZIP_NO_LOADAVG
	fprintf(stderr, "Usage: %s [-cdfhklm#p#qrS#tz] <filename> <filename2> <filenameN>\n", progname);
#else
	fprintf(stderr, "Usage: %s  [cdfhkm#p#qrS#tz] <filename> <filename2> <filenameN>\n", progname);
#endif // PBZIP_NO_LOADAVG
    /*
	 *fprintf(stderr, " -1 .. -9        set BWT block size to 100k .. 900k (default 900k)\n");
     */
    /*
	 *fprintf(stderr, " -b#             Block size in 100k steps (default 9 = 900k)\n");
     */
    /*
	 *fprintf(stderr, " -c,--stdout     Output to standard out (stdout)\n");
     */
	fprintf(stderr, " -d,--decompress Decompress file\n");
	fprintf(stderr, " -f,--force      Overwrite existing output file\n");
	fprintf(stderr, " -h,--help       Print this help message\n");
	fprintf(stderr, " -k,--keep       Keep input file, don't delete\n");
#ifndef PBZIP_NO_LOADAVG
	fprintf(stderr, " -l,--loadavg    Load average determines max number processors to use\n");
#endif // PBZIP_NO_LOADAVG
	fprintf(stderr, " -m#             Maximum memory usage in 1MB steps (default 100 = 100MB)\n");
	fprintf(stderr, " -p#             Number of processors to use (default");
#if defined(_SC_NPROCESSORS_ONLN) || defined(__APPLE__)
	fprintf(stderr, ": autodetect [%d])\n", detectCPUs());
#else
	fprintf(stderr, " 2)\n");
#endif // _SC_NPROCESSORS_ONLN || __APPLE__
	fprintf(stderr, " -q,--quiet      Quiet mode (default)\n");
	fprintf(stderr, " -r,--read       Read entire input file into RAM and split between processors\n");
#ifdef USE_STACKSIZE_CUSTOMIZATION
	fprintf(stderr, " -S#             Child thread stack size in 1KB steps (default stack size if unspecified)\n");
#endif // USE_STACKSIZE_CUSTOMIZATION
	fprintf(stderr, " -t,--test       Test compressed file integrity\n");
	fprintf(stderr, " -v,--verbose    Verbose mode\n");
    /*
	 *fprintf(stderr, " -V,--version    Display version info for pbzip2 then exit\n");
     */
	fprintf(stderr, " -z,--compress   Compress file (default)\n");
    /*
	 *fprintf(stderr, " --ignore-trailing-garbage=# Ignore trailing garbage flag (1 - ignored; 0 - forbidden)\n");
	 *fprintf(stderr, "\n");
	 *fprintf(stderr, "If no file names are given, pbzip2 compresses or decompresses from standard input to standard output.\n");
	 *fprintf(stderr, "\n");
	 *fprintf(stderr, "Example: pbzip2 -b15vk myfile.tar\n");
	 *fprintf(stderr, "Example: pbzip2 -p4 -r -5 myfile.tar second*.txt\n");
	 *fprintf(stderr, "Example: tar cf myfile.tar.bz2 --use-compress-prog=pbzip2 dir_to_compress/\n");
	 *fprintf(stderr, "Example: pbzip2 -d -m500 myfile.tar.bz2\n");
	 *fprintf(stderr, "Example: pbzip2 -dc myfile.tar.bz2 | tar x\n");
	 *fprintf(stderr, "Example: pbzip2 -c < myfile.txt > myfile.txt.bz2 \n");
     */
	fprintf(stderr, "\n");
	exit(-1);
}

typedef struct {
    int argc;
    char **argv;
}Argument_t;




Argument_t getOutputDir(int argc, char **argv, char *outputDir)
{
    Argument_t args;
    int maxArgumentLen = 5120;
    args.argv = (char **)malloc(argc * sizeof(char *));
    for(int i = 0; i < argc; i ++)
    {
        args.argv[i] = (char *)malloc(maxArgumentLen);
        memset(args.argv[i], '\0', maxArgumentLen);
    }
    
    int index = 0;
    int nextStart = 0;
    int finalArgNum = 0;

    std::vector<string> v;
    for(int i = 0; i < argc; i ++)
    {
        

        if(strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output_dir") == 0)
        {
            sprintf(outputDir, "%s", argv[i + 1]);
            /*
             * Jump to option behind the value of --output_dir
             */
            i = i + 1;
        }
        else
        {
            v.push_back(string(argv[i]));
        }

    }


    int n = v.size();
    for(int i = 0; i < n; i ++)
    {
        strcpy(args.argv[i], v[i].c_str());
    }

    args.argc = n;

   return args;
}

std::string getFileNameFromPath(std::string const & path)
{
    return path.substr(path.find_last_of("/\\") + 1);
}

int isMRCFile(std::string& fileBaseName)
{
    size_t pos = fileBaseName.find_last_of('.');
    int isMRC = 0;
    if(pos != std::string::npos)
    {
        std::string suffix = fileBaseName.substr(pos + 1);
        if(suffix == "mrc" || suffix == "mrcs")
        {
            isMRC = 1;
        }
    }

    return isMRC;
}
/*
 *********************************************************
 */
int main(int iargc, char* iargv[])
{

    char outputDir[2048];
    memset(outputDir, '\0', sizeof(outputDir));
    Argument_t args = getOutputDir(iargc, iargv, outputDir);
    int argc = args.argc;
    char **argv = args.argv;

    if(strlen(outputDir) > 0 && outputDir[strlen(outputDir) - 1] != '/')
    {
        outputDir[strlen(outputDir)] = '/';
    }
    /*
     * Ouput to current directory in default
     */
    if(strlen(outputDir) == 0)
    {
        strcpy(outputDir, "./");
    }

	queue *fifo;
	pthread_t output;
	char **FileList = NULL;
	char *InFilename = NULL;
	bool hasInFile = false;
	char *progName = NULL;
	char *progNamePos = NULL;
	char bz2Header[] = {"BZh91AY&SY"};  // using 900k block size
	std::string outFilename; // [2048];
	char cmdLineTemp[2048];
	unsigned char tmpBuff[50];
	char stdinFile[2] = {"-"};
	struct timeval tvStartTime;
	struct timeval tvStopTime;
	#ifndef WIN32
	struct timezone tz;
	double loadAverage = 0.0;
	double loadAvgArray[3];
	int useLoadAverage = 0;
	int numCPUtotal = 0;
	int numCPUidle = 0;
	#else
	SYSTEMTIME systemtime;
	LARGE_INTEGER filetime;
	LARGE_INTEGER fileSize_temp;
	HANDLE hInfile_temp;
	#endif
	double timeCalc = 0.0;
	double timeStart = 0.0;
	double timeStop = 0.0;
	int cmdLineTempCount = 0;
	int readEntireFile = 0;
	int zeroByteFile = 0;
	int hInfile = -1;
	int hOutfile = -1;
	int numBlocks = 0;
	int blockSize = 9*100000;
	size_t maxMemory = 100000000;
	int maxMemorySwitch = 0;
	int decompress = 0;
	int compress = 0;
	int testFile = 0;
	int errLevel = 0;
	int noThreads = 0;
	int keep = 0;
	int force = 0;
	int ret = 0;
	int fileLoop;
	size_t i, j, k;
	bool switchedMtToSt = false; // switched from multi- to single-thread

	// Initialize error context
	if (ErrorContext::getInstance() == NULL)
	{
		return 1;
	}

	// get current time for benchmark reference
	#ifndef WIN32
	gettimeofday(&tvStartTime, &tz);
	#else
	GetSystemTime(&systemtime);
	SystemTimeToFileTime(&systemtime, (FILETIME *)&filetime);
	tvStartTime.tv_sec = filetime.QuadPart / 10000000;
	tvStartTime.tv_usec = (filetime.QuadPart - (LONGLONG)tvStartTime.tv_sec * 10000000) / 10;
	#endif

	// check to see if we are likely being called from TAR
	if (argc < 2)
	{
		OutputStdOut = 1;
		keep = 1;
	}

	// get program name to determine if decompress mode should be used
	progName = argv[0];
	for (progNamePos = argv[0]; progNamePos[0] != '\0'; progNamePos++)
	{
		if (progNamePos[0] == PATH_SEP)
			progName = progNamePos + 1;
	}
	if ((strstr(progName, "unzip") != 0) || (strstr(progName, "UNZIP") != 0))
	{
		decompress = 1;
	}
	if ((strstr(progName, "zcat") != 0) || (strstr(progName, "ZCAT") != 0))
	{
		decompress = OutputStdOut = keep = 1; 
	}

	#ifdef IGNORE_TRAILING_GARBAGE
	// default behavior is hard-coded (still dynamically changeable)
	IgnoreTrailingGarbageFlag = IGNORE_TRAILING_GARBAGE;
	#else
	// default depends on program name
	if ((strcmp(progName, "bzip2") == 0) || (strcmp(progName, "BZIP2") == 0) ||
		(strcmp(progName, "bunzip2") == 0) || (strcmp(progName, "BUNZIP2") == 0) ||
		(strcmp(progName, "bzcat") == 0) || (strcmp(progName, "BZCAT") == 0))
	{
		// Favour traditional non-parallel bzip2 behavior
		IgnoreTrailingGarbageFlag = 1;
	}
	#endif // IGNORE_TRAILING_GARBAGE
	
	FileListCount = 0;
	FileList = new(std::nothrow) char *[argc];
	if (FileList == NULL)
	{
		fprintf(stderr, "pbz2: *ERROR: Not enough memory!  Aborting...\n");
		return 1;
	}
	// set default max memory usage to 100MB
	maxMemory = 100000000;
	NumBufferedBlocksMax = 0;

	numCPU = detectCPUs();

	#ifndef WIN32
	numCPUtotal = numCPU;
	#endif

	// parse command line switches
	for (i=1; (int)i < argc; i++)
	{
		if (argv[i][0] == '-')
		{
			if (argv[i][1] == '\0') 
			{
				// support "-" as a filename
				FileList[FileListCount] = argv[i];
				FileListCount++;
				continue;
			}
			else if (argv[i][1] == '-')
			{
				// get command line options with "--"
				if (strcmp(argv[i], "--best") == 0)
				{
					BWTblockSize = 9;
				}
				else if (strcmp(argv[i], "--decompress") == 0)
				{
					decompress = 1;
				}
				else if (strcmp(argv[i], "--compress") == 0)
				{
					compress = 1;
				}
				else if (strcmp(argv[i], "--fast") == 0)
				{
					BWTblockSize = 1;
				}
				else if (strcmp(argv[i], "--force") == 0)
				{
					force = 1; ForceOverwrite = 1;
				}
				else if (strcmp(argv[i], "--help") == 0)
				{
					usage(argv[0], "HELP");
				}
				else if (strcmp(argv[i], "--keep") == 0)
				{
					keep = 1;
				}
				else if (strcmp(argv[i], "--license") == 0)
				{
					usage(argv[0], "HELP");
				}
				#ifndef PBZIP_NO_LOADAVG
				else if (strcmp(argv[i], "--loadavg") == 0)
				{
					useLoadAverage = 1;
				}
				#endif
				else if (strcmp(argv[i], "--quiet") == 0)
				{
					QuietMode = 1;
				}
				else if (strcmp(argv[i], "--read") == 0)
				{
					readEntireFile = 1;
				}
				else if (strcmp(argv[i], "--stdout") == 0)
				{
					OutputStdOut = 1; keep = 1;
				}
				else if (strcmp(argv[i], "--test") == 0)
				{
					testFile = 1;
				}
				else if (strcmp(argv[i], "--verbose") == 0)
				{
					QuietMode = 0;
				}
				else if (strcmp(argv[i], "--version") == 0)
				{
					banner(); exit(0);
				}
				else if (strcmp(argv[i], "--ignore-trailing-garbage") == 0 )
				{
					IgnoreTrailingGarbageFlag = 1;
				}
				else if (strcmp(argv[i], "--ignore-trailing-garbage=1") == 0 )
				{
					IgnoreTrailingGarbageFlag = 1;
				}
				else if (strcmp(argv[i], "--ignore-trailing-garbage=0") == 0 )
				{
					IgnoreTrailingGarbageFlag = 0;
				}
				
				continue;
			}
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "argv[%u]: %s   Len: %d\n", (unsigned)i, argv[i], (int)strlen(argv[i]));
			#endif
			// get command line options with single "-"
			// check for multiple switches grouped together
			for (j=1; argv[i][j] != '\0'; j++)
			{
				switch (argv[i][j])
				{
				case 'p': k = j+1; cmdLineTempCount = 0; strcpy(cmdLineTemp, "2");
					while (argv[i][k] != '\0' && k < sizeof(cmdLineTemp))
					{
						// no more numbers, finish
						if ((argv[i][k] < '0') || (argv[i][k] > '9'))
							break;
						k++;
						cmdLineTempCount++;
					}
					if (cmdLineTempCount == 0)
						usage(argv[0], "Cannot parse -p argument");
					strncpy(cmdLineTemp, argv[i]+j+1, cmdLineTempCount);
					cmdLineTemp[cmdLineTempCount] = '\0';
					numCPU = atoi(cmdLineTemp);
					if (numCPU > 4096)
					{
						fprintf(stderr,"pbz2: *ERROR: Maximal number of supported processors is 4096!  Aborting...\n");
						return 1;
					}
					else if (numCPU < 1)
					{
						fprintf(stderr,"pbz2: *ERROR: Minimum number of supported processors is 1!  Aborting...\n");
						return 1;
					}
					j += cmdLineTempCount;
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "-p%d\n", numCPU);
					#endif
					break;
				case 'b': k = j+1; cmdLineTempCount = 0; strcpy(cmdLineTemp, "9"); blockSize = 900000;
					while (argv[i][k] != '\0' && k < sizeof(cmdLineTemp))
					{
						// no more numbers, finish
						if ((argv[i][k] < '0') || (argv[i][k] > '9'))
							break;
						k++;
						cmdLineTempCount++;
					}
					if (cmdLineTempCount == 0)
						usage(argv[0], "Cannot parse file block size");
					strncpy(cmdLineTemp, argv[i]+j+1, cmdLineTempCount);
					cmdLineTemp[cmdLineTempCount] = '\0';
					blockSize = atoi(cmdLineTemp)*100000;
					if ((blockSize < 100000) || (blockSize > 1000000000))
					{
						fprintf(stderr,"pbz2: *ERROR: File block size Min: 100k and Max: 1000000k!  Aborting...\n");
						return 1;
					}
					j += cmdLineTempCount;
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "-b%d\n", blockSize);
					#endif
					break;
				case 'm': k = j+1; cmdLineTempCount = 0; strcpy(cmdLineTemp, "1"); maxMemory = 1000000;
					while (argv[i][k] != '\0' && k < sizeof(cmdLineTemp))
					{
						// no more numbers, finish
						if ((argv[i][k] < '0') || (argv[i][k] > '9'))
							break;
						k++;
						cmdLineTempCount++;
					}
					if (cmdLineTempCount == 0)
						usage(argv[0], "Cannot parse -m argument");
					strncpy(cmdLineTemp, argv[i]+j+1, cmdLineTempCount);
					cmdLineTemp[cmdLineTempCount] = '\0';
					maxMemory = ((size_t)atoi(cmdLineTemp))*1000000;
                    /*
                     *fprintf(stderr,"cmdLineTemp = %s\n", cmdLineTemp);
                     *fprintf(stderr, "maxMemory = %lu\n", maxMemory);
                     *size_t M = 8000000000;
                     *fprintf(stderr, "%d:%d\n", maxMemory < 1000000, maxMemory > M);
                     *fprintf(stderr, "sizeof(long long) : %d\n", sizeof(long long));
                     *fprintf(stderr, "sizeof(size_t) : %d\n", sizeof(size_t));
                     *fprintf(stderr, "sizeof(long) : %d\n", sizeof(long));
                     */
					if ((maxMemory < 1000000) || (maxMemory > 16000000000))
					{
						fprintf(stderr,"pbz2:: *ERROR2: Memory usage size Min: 1MB and Max: 16GB!  Aborting...\n");
						return 1;
					}
					maxMemorySwitch = 1;
					j += cmdLineTempCount;
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "-m%d\n", maxMemory);
					#endif
					break;
				#ifdef USE_STACKSIZE_CUSTOMIZATION
				case 'S': k = j+1; cmdLineTempCount = 0; strcpy(cmdLineTemp, "0"); ChildThreadStackSize = -1;
					while (argv[i][k] != '\0' && k < sizeof(cmdLineTemp))
					{
						// no more numbers, finish
						if ((argv[i][k] < '0') || (argv[i][k] > '9'))
							break;
						k++;
						cmdLineTempCount++;
					}
					if (cmdLineTempCount == 0)
						usage(argv[0], "Cannot parse -S argument");
					strncpy(cmdLineTemp, argv[i]+j+1, cmdLineTempCount);
					cmdLineTemp[cmdLineTempCount] = '\0';
					ChildThreadStackSize = atoi(cmdLineTemp)*1024;
					if (ChildThreadStackSize < 0)
					{
						fprintf(stderr,"pbz2: *ERROR: Parsing -S: invalid stack size specified [%d]!  Ignoring...\n",
							 ChildThreadStackSize);
					}
					else if (ChildThreadStackSize < PTHREAD_STACK_MIN)
					{
						fprintf(stderr,"pbz2: *WARNING: Stack size %d bytes less than minumum - adjusting to %d bytes.\n",
							 ChildThreadStackSize, PTHREAD_STACK_MIN);
						ChildThreadStackSize = PTHREAD_STACK_MIN;
					}
					j += cmdLineTempCount;
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "-S%d\n", ChildThreadStackSize);
					#endif
					break;
				#endif // USE_STACKSIZE_CUSTOMIZATION
				case 'd': decompress = 1; break;
				case 'c': OutputStdOut = 1; keep = 1; break;
				case 'f': force = 1; ForceOverwrite = 1; break;
				case 'h': usage(argv[0], "HELP"); break;
				case 'k': keep = 1; break;
				#ifndef PBZIP_NO_LOADAVG
				case 'l': useLoadAverage = 1; break;
				#endif
				case 'L': banner(); exit(0); break;
				case 'q': QuietMode = 1; break;
				case 'r': readEntireFile = 1; break;
				case 't': testFile = 1; break;
				case 'v': QuietMode = 0; break;
				case 'V': banner(); exit(0); break;
				case 'z': compress = 1; break;
				case '1': BWTblockSize = 1; break;
				case '2': BWTblockSize = 2; break;
				case '3': BWTblockSize = 3; break;
				case '4': BWTblockSize = 4; break;
				case '5': BWTblockSize = 5; break;
				case '6': BWTblockSize = 6; break;
				case '7': BWTblockSize = 7; break;
				case '8': BWTblockSize = 8; break;
				case '9': BWTblockSize = 9; break;
				}
			}
		}
		else
		{
			// add filename to list for processing FileListCount
			FileList[FileListCount] = argv[i];
			FileListCount++;
		}
	} /* for */

	Bz2HeaderZero[3] = '0' + BWTblockSize;
	bz2Header[3] = Bz2HeaderZero[3];

	// check to make sure we are not trying to compress and decompress at same time
	if ((compress == 1) && (decompress == 1))
	{
		fprintf(stderr,"pbz2: *ERROR: Can't compress and uncompress data at same time.  Aborting!\n");
		fprintf(stderr,"pbz2: For help type: %s -h\n", argv[0]);
		return 1;
	}

	if (FileListCount == 0)
	{
		if (testFile == 1)
		{
			#ifndef WIN32
			if (isatty(fileno(stdin)))
			#else
			if (_isatty(_fileno(stdin)))
			#endif
			{
					fprintf(stderr,"pbz2: *ERROR: Won't read compressed data from terminal.  Aborting!\n");
					fprintf(stderr,"pbz2: For help type: %s -h\n", argv[0]);
					return 1;
			}
			// expecting data from stdin
			FileList[FileListCount] = stdinFile;
			FileListCount++;
		}
		else if (decompress == 1)
		{
			#ifndef WIN32
			if (isatty(fileno(stdin)))
			#else
			if (_isatty(_fileno(stdin)))
			#endif
			{
				fprintf(stderr,"pbz2: *ERROR: Won't read compressed data from terminal.  Aborting!\n");
				fprintf(stderr,"pbz2: For help type: %s -h\n", argv[0]);
				return 1;
			}
			// expecting data from stdin via TAR
			OutputStdOut = 1;
			keep = 1;
			FileList[FileListCount] = stdinFile;
			FileListCount++;
		}
		else
		{
			if (OutputStdOut == 0)
			{
				// probably trying to input data from stdin
				if (QuietMode != 1)
					fprintf(stderr,"pbz2: Assuming input data coming from stdin...\n\n");
				
				OutputStdOut = 1;
				keep = 1;
			}

			#ifndef WIN32
			if (isatty(fileno(stdout)))
			#else
			if (_isatty(_fileno(stdout)))
			#endif
			{
				fprintf(stderr,"pbz2: *ERROR: Won't write compressed data to terminal.  Aborting!\n");
				fprintf(stderr,"pbz2: For help type: %s -h\n", argv[0]);
				return 1;
			}
			// expecting data from stdin
			FileList[FileListCount] = stdinFile;
			FileListCount++;
		}
	}

    

	if (QuietMode != 1)
	{
		// display program banner
		banner();

		// do sanity check to make sure integers are the size we expect
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "off_t size: %u    uint size: %u\n", (unsigned)sizeof(OFF_T), (unsigned)sizeof(unsigned int));
		#endif
		if (sizeof(OFF_T) <= 4)
		{
			fprintf(stderr, "\npbzip2: *WARNING: off_t variable size only %u bits!\n", (unsigned)(sizeof(OFF_T)*CHAR_BIT));
			if (decompress == 1)
				fprintf(stderr, " You will only able to uncompress files smaller than 2GB in size.\n\n");
			else
				fprintf(stderr, " You will only able to compress files smaller than 2GB in size.\n\n");
		}
	}
	
	// Calculate number of processors to use based on load average if requested
	#ifndef PBZIP_NO_LOADAVG
	if (useLoadAverage == 1)
	{
		// get current load average
		ret = getloadavg(loadAvgArray, 3);
		if (ret != 3)
		{
			loadAverage = 0.0;
			useLoadAverage = 0;
			if (QuietMode != 1)
				fprintf(stderr, "pbz2:  *WARNING: Could not get load average!  Using requested processors...\n");
		}
		else
		{
			#ifdef PBZIP_DEBUG
			fprintf(stderr, "Load Avg1: %f  Avg5: %f  Avg15: %f\n", loadAvgArray[0], loadAvgArray[1], loadAvgArray[2]);
			#endif
			// use 1 min load average to adjust number of processors used
			loadAverage = loadAvgArray[0]; // use [1] for 5 min average and [2] for 15 min average
			// total number processors minus load average rounded up
			numCPUidle = numCPUtotal - (int)(loadAverage + 0.5);
			// if user asked for a specific # processors and they are idle, use all requested
			// otherwise give them whatever idle processors are available
			if (numCPUidle < numCPU)
				numCPU = numCPUidle;
			if (numCPU < 1)
				numCPU = 1;
		}
	}
	#endif

	// Initialize child threads attributes
	initChildThreadAttributes();

	// setup signal handling (should be before creating any child thread)
	sigInFilename = NULL;
	sigOutFilename = NULL;
	ret = setupSignalHandling();
	if (ret != 0)
	{
		fprintf(stderr, "pbz2: *ERROR: Can't setup signal handling [%d]. Aborting!\n", ret);
		return 1;
	}

	// Create and start terminator thread.
	ret = setupTerminator();
	if (ret != 0)
	{
		fprintf(stderr, "pbz2: *ERROR: Can't setup terminator thread [%d]. Aborting!\n", ret);
		return 1;
	}

	if (numCPU < 1)
		numCPU = 1;

	// display global settings
	if (QuietMode != 1)
	{
		if (testFile != 1)
		{
			fprintf(stderr, "\n         # CPUs: %d\n", numCPU);
			#ifndef PBZIP_NO_LOADAVG
			if (useLoadAverage == 1)
				fprintf(stderr, "   Load Average: %.2f\n", loadAverage);
			#endif
			if (decompress != 1)
			{
				fprintf(stderr, " BWT Block Size: %d00 KB\n", BWTblockSize);
				if (blockSize < 100000)
					fprintf(stderr, "File Block Size: %d bytes\n", blockSize);
				else
					fprintf(stderr, "File Block Size: %d KB\n", blockSize/1000);
			}
			fprintf(stderr, " Maximum Memory: %d MB\n", maxMemory/1000000);
			#ifdef USE_STACKSIZE_CUSTOMIZATION
				if (ChildThreadStackSize > 0)
					fprintf(stderr, "     Stack Size: %d KB\n", ChildThreadStackSize/1024);
			#endif

			if (decompress == 1)
			{
				fprintf(stderr, " Ignore Trailing Garbage: %s\n",
					 (IgnoreTrailingGarbageFlag == 1) ? "on" : "off" );
			}
		}
		fprintf(stderr, "-------------------------------------------\n");
	}

	int mutexesInitRet = mutexesInit();
	if ( mutexesInitRet != 0 )
	{
		return mutexesInitRet;
	}

	// create queue
	fifo = FifoQueue = queueInit(numCPU);
	if (fifo == NULL)
	{
		fprintf (stderr, "pbz2: *ERROR: Queue Init failed.  Aborting...\n");
		return 1;
	}

    //Add by huabin
    for(int i = 0; i < FileListCount; i ++)
    {
        fprintf(stderr, "%d:%s\n", i, FileList[i]);
    }


	// process all files
	for (fileLoop=0; fileLoop < FileListCount; fileLoop++)
	{
		producerDone = 0;
		InFileSize = 0;
		NumBlocks = 0;
		switchedMtToSt = false;
		int errLevelCurrentFile = 0;
		
		ErrorContext::getInstance()->reset();

		// set input filename
		InFilename = FileList[fileLoop];
		hasInFile = (strcmp(InFilename, "-") != 0);

		// test file for errors if requested
		if (testFile != 0)
		{
			if (QuietMode != 1)
			{
				fprintf(stderr, "      File #: %d of %d\n", fileLoop+1, FileListCount);
				if (hasInFile)
					fprintf(stderr, "     Testing: %s\n", InFilename);
				else
					fprintf(stderr, "     Testing: <stdin>\n");
			}
			ret = testCompressedData(InFilename);
			if (ret > 0)
				return ret;
			else if (ret == 0)
			{
				if (QuietMode != 1)
					fprintf(stderr, "        Test: OK\n");
			}
			else
				errLevel = 2;

			if (QuietMode != 1)
				fprintf(stderr, "-------------------------------------------\n");
			continue;
		}

		// set ouput filename
        std::string outputDirStr(outputDir);
        std::string currFileBaseName = getFileNameFromPath(FileList[fileLoop]);
        outFilename = outputDirStr + currFileBaseName;
		//outFilename = std::string(outputDirStr + FileList[fileLoop]);
        
		if ((decompress == 1) && hasInFile)
		{
			// check if input file is a valid .bz2 compressed file
			hInfile = open(InFilename, O_RDONLY | O_BINARY);
			// check to see if file exists before processing
			if (hInfile == -1)
			{
				ErrorContext::printErrnoMsg(stderr, errno);
				fprintf(stderr, "pbz2: *ERROR: File [%s] NOT found!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			memset(tmpBuff, 0, sizeof(tmpBuff));
			size_t size = do_read(hInfile, tmpBuff, strlen(bz2Header)+1);
			do_close(hInfile);
			if ((size == (size_t)(-1)) || (size < strlen(bz2Header)+1))
			{
				ErrorContext::getInstance()->printErrorMessages(stderr);
				fprintf(stderr, "pbz2: *ERROR: File [%s] is NOT a valid bzip2!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			else
			{
				// make sure start of file has valid bzip2 header
				if (memstr(tmpBuff, 4, bz2Header, 3) == NULL)
				{
					fprintf(stderr, "pbz2: *ERROR: File [%s] is NOT a valid bzip2!  Skipping...\n", InFilename);
					fprintf(stderr, "-------------------------------------------\n");
					errLevel = 1;
					continue;
				}
				// skip 4th char which differs depending on BWT block size used
				if (memstr(tmpBuff+4, size-4, bz2Header+4, strlen(bz2Header)-4) == NULL)
				{
					// check to see if this is a special 0 byte file
					if (memstr(tmpBuff+4, size-4, Bz2HeaderZero+4, strlen(bz2Header)-4) == NULL)
					{
						fprintf(stderr, "pbz2: *ERROR: File [%s] is NOT a valid bzip2!  Skipping...\n", InFilename);
						fprintf(stderr, "-------------------------------------------\n");
						errLevel = 1;
						continue;
					}
					#ifdef PBZIP_DEBUG
					fprintf(stderr, "** ZERO byte compressed file detected\n");
					#endif
				}
				// set block size for decompression
				if ((tmpBuff[3] >= '1') && (tmpBuff[3] <= '9'))
					BWTblockSizeChar = tmpBuff[3];
				else
				{
					fprintf(stderr, "pbz2: *ERROR: File [%s] is NOT a valid bzip2!  Skipping...\n", InFilename);
					fprintf(stderr, "-------------------------------------------\n");
					errLevel = 1;
					continue;
				}
			}

			// check if filename ends with .bz2
			std::string bz2Tail(".bz2");
			std::string tbz2Tail(".tbz2");
			if ( ends_with_icase(outFilename, bz2Tail) )
			{
				// remove .bz2 extension
				outFilename.resize( outFilename.size() - bz2Tail.size() );
			}
			else if ( ends_with_icase(outFilename, tbz2Tail) )
			{
				outFilename.resize( outFilename.size() - tbz2Tail.size() );
				outFilename += ".tar";
			}
			else
			{
				// add .out extension so we don't overwrite original file
				outFilename += ".out";
			}
		} // decompress == 1
		else
		{
			// check input file to make sure its not already a .bz2 file
			std::string bz2Tail(".bz2");
			if ( ends_with_icase(std::string(InFilename), bz2Tail) )
			{
				fprintf(stderr, "pbz2: *ERROR: Input file [%s] already has a .bz2 extension!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			outFilename += bz2Tail;
		}

		// setup signal handling filenames
		safe_mutex_lock(&ErrorHandlerMutex);
		sigInFilename = InFilename;
		sigOutFilename = outFilename.c_str();
		safe_mutex_unlock(&ErrorHandlerMutex);

		if (hasInFile)
		{
			struct stat statbuf;
			// read file for compression
			hInfile = open(InFilename, O_RDONLY | O_BINARY);
			// check to see if file exists before processing
			if (hInfile == -1)
			{
				fprintf(stderr, "pbz2: *ERROR: File [%s] NOT found!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}

            /* 
             *Here we only process the file with suffix: mrc or mrcs 
             */
            /*
             *fprintf(stderr, "Start to process file: %s\n", InFilename);
             */
            int isMRC = isMRCFile(currFileBaseName);
            if(!isMRC)
            {
                fprintf(stderr, "pbz2: *ERROR: File [%s] is  not type of  mrc or mrcs!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
            }



			// get some information about the file
			fstat(hInfile, &statbuf);
			// check to make input is not a directory
			if (S_ISDIR(statbuf.st_mode))
			{
				fprintf(stderr, "pbz2: *ERROR: File [%s] is a directory!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			// check to make sure input is a regular file
			if (!S_ISREG(statbuf.st_mode))
			{
				fprintf(stderr, "pbz2: *ERROR: File [%s] is not a regular file!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			// get size of file
			#ifndef WIN32
			InFileSize = statbuf.st_size;
			#else
			fileSize_temp.LowPart = GetFileSize((HANDLE)_get_osfhandle(hInfile), (unsigned long *)&fileSize_temp.HighPart);
			InFileSize = fileSize_temp.QuadPart;
			#endif
			// don't process a 0 byte file
			if (InFileSize == 0)
			{
				if (decompress == 1)
				{
					fprintf(stderr, "pbz2: *ERROR: File is of size 0 [%s]!  Skipping...\n", InFilename);
					fprintf(stderr, "-------------------------------------------\n");
					errLevel = 1;
					continue;
				}
				
				// make sure we handle zero byte files specially
				zeroByteFile = 1;
			}
			else
			{
				zeroByteFile = 0;
			}

			// get file meta data to write to output file
			if (getFileMetaData(InFilename) != 0)
			{
				fprintf(stderr, "pbz2: *ERROR: Could not get file meta data from [%s]!  Skipping...\n", InFilename);
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
		}
		else
		{
			hInfile = STDIN_FILENO; // stdin
			InFileSize = -1; // fake it
		}

		// check to see if output file exists
		if ((OutputStdOut == 0) && check_file_exists(outFilename.c_str()))
		{
			if (force != 1)
			{
				fprintf(stderr, "pbz2: *ERROR: Output file [%s] already exists!  Use -f to overwrite...\n", outFilename.c_str());
				fprintf(stderr, "-------------------------------------------\n");
				errLevel = 1;
				continue;
			}
			else
			{
				remove(outFilename.c_str());
			}
		}

		if (readEntireFile == 1)
		{
			if (hInfile == STDIN_FILENO) 
			{
				if (QuietMode != 1)
					fprintf(stderr, " *Warning: Ignoring -r switch since input is stdin.\n");
			}
			else
			{
				// determine block size to try and spread data equally over # CPUs
				blockSize = InFileSize / numCPU;
			}
		}

		// display per file settings
		if (QuietMode != 1)
		{
			fprintf(stderr, "         File #: %d of %d\n", fileLoop+1, FileListCount);
			fprintf(stderr, "     Input Name: %s\n", hInfile != STDIN_FILENO ? InFilename : "<stdin>");

			if (OutputStdOut == 0)
				fprintf(stderr, "    Output Name: %s\n\n", outFilename.c_str());
			else
				fprintf(stderr, "    Output Name: <stdout>\n\n");

			if (decompress == 1)
				fprintf(stderr, " BWT Block Size: %c00k\n", BWTblockSizeChar);
			if (hasInFile)
				fprintf(stderr, "     Input Size: %"PRIuMAX" bytes\n", (uintmax_t)InFileSize);
		}

		if (decompress == 1)
		{
			numBlocks = 0;
			// Do not use threads if we only have 1 CPU or small files
			if ((numCPU == 1) || (InFileSize < 1000000))
				noThreads = 1;
			else
				noThreads = 0;
			
			// Enable threads method for uncompressing from stdin
			if ((numCPU > 1) && !hasInFile)
				noThreads = 0;
		} // if (decompress == 1)
		else
		{
			if (InFileSize > 0)
			{
				// calculate the # of blocks of data
				numBlocks = (InFileSize + blockSize - 1) / blockSize;
				// Do not use threads for small files where we only have 1 block to process
				// or if we only have 1 CPU
				if ((numBlocks == 1) || (numCPU == 1))
					noThreads = 1;
				else
					noThreads = 0;
			} 
			else 
			{
				// Simulate a "big" number of buffers. Will need to resize it later
				numBlocks = 10000;
			}
			
			// write special compressed data for special 0 byte input file case
			if (zeroByteFile == 1)
			{
				hOutfile = STDOUT_FILENO;
				// write to file instead of stdout
				if (OutputStdOut == 0)
				{
					hOutfile = safe_open_output(outFilename.c_str());
					// check to see if file creation was successful
					if (hOutfile == -1)
					{
						handle_error(EF_EXIT, 1,
							"pbz2: *ERROR: Could not create output file [%s]!\n", outFilename.c_str());
						errLevelCurrentFile = errLevel = 1;
						break;
					}
				}
				// write data to the output file
				ret = do_write(hOutfile, Bz2HeaderZero, sizeof(Bz2HeaderZero));
				int close_ret = 0;
				if (OutputStdOut == 0)
				{
					close_ret = do_close(hOutfile);
					// write store file meta data to output file
					if (writeFileMetaData(outFilename.c_str()) != 0)
					{
						handle_error(EF_NOQUIT, -1,
							"pbz2: *ERROR: Could not write file meta data to [%s]!\n", outFilename.c_str());
					}
				}
				if ( (ret != sizeof(Bz2HeaderZero)) || (close_ret == -1) )
				{
					handle_error(EF_EXIT, 1,
						"pbz2: *ERROR: Could not write to file [%s]! Aborting...\n", outFilename.c_str());
					fprintf(stderr, "-------------------------------------------\n");
					errLevelCurrentFile = errLevel = 1;
					break;
				}
				if (QuietMode != 1)
				{
					fprintf(stderr, "    Output Size: %u bytes\n", (unsigned)sizeof(Bz2HeaderZero));
					fprintf(stderr, "-------------------------------------------\n");
				}
				// remove input file unless requested not to by user or error occurred
				if ( (keep != 1) && (errLevelCurrentFile == 0) )
				{
					struct stat statbuf;
					// only remove input file if output file exists
					bool removeFlag =
							(OutputStdOut != 0) ||
							(stat(outFilename.c_str(), &statbuf) == 0);
					
					if (removeFlag)
					{
						if (do_remove(InFilename) == -1)
						{
							handle_error(EF_NOQUIT, 1, "Can't remove input file [%s]!", InFilename);
						}
					}
				}
				continue;
			} // if (zeroByteFile == 1)
		} // else (decompress == 1)
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "# Blocks: %d\n", numBlocks);
		#endif
		// set global variable
		NumBlocksEstimated = numBlocks;
		// Calculate maximum number of buffered blocks to use
		NumBufferedBlocksMax = maxMemory / blockSize;
		// Subtract blocks for number of extra buffers in producer and fileWriter (~ numCPU for each)
		if ((int)NumBufferedBlocksMax - (numCPU * 2) < 1)
			NumBufferedBlocksMax = 1;
		else
			NumBufferedBlocksMax = NumBufferedBlocksMax - (numCPU * 2);
		#ifdef PBZIP_DEBUG
		fprintf(stderr, "pbz2: maxMemory: %d    blockSize: %d\n", maxMemory, blockSize);
		fprintf(stderr, "pbz2: NumBufferedBlocksMax: %"PRIuMAX"\n", (uintmax_t)NumBufferedBlocksMax);
		#endif
		// check to see if our max buffered blocks is less than numCPU, if yes increase maxMemory
		// to support numCPU requested unless -m switch given by user
		if (NumBufferedBlocksMax < (size_t)numCPU)
		{
			if (maxMemorySwitch == 0)
			{
				NumBufferedBlocksMax = numCPU;
				if (QuietMode != 1)
					fprintf(stderr, "*Warning* Max memory limit increased to %"PRIuMAX" MB to support %d CPUs\n", (uintmax_t)((NumBufferedBlocksMax + (numCPU * 2)) * blockSize)/1000000, numCPU);
			}
			else
			{
				if (QuietMode != 1)
					fprintf(stderr, "*Warning* CPU usage and performance may be suboptimal due to max memory limit.\n");
			}
		}

		LastGoodBlock = -1;
		MinErrorBlock = -1;
		
		// create output buffer
		outputBufferInit(NumBufferedBlocksMax);

		if (decompress == 1)
		{
			// use multi-threaded code
			if (noThreads == 0)
			{
				// do decompression
				if (QuietMode != 1)
					fprintf(stderr, "Decompressing data...\n");
				for (i=0; (int)i < numCPU; i++)
				{
					ret = pthread_create(&fifo->consumers[i], &ChildThreadAttributes, consumer_decompress, fifo);
					if (ret != 0)
					{
						ErrorContext::getInstance()->saveError();
						handle_error(EF_EXIT, 1, "pbz2: *ERROR: Not enough resources to create consumer thread #%u (code = %d)  Aborting...\n", i, ret);
						ret = pthread_join(TerminatorThread, NULL);
						return 1;
					}
				}

				ret = pthread_create(&output, &ChildThreadAttributes, fileWriter, (void*)outFilename.c_str());
				if (ret != 0)
				{
					handle_error(EF_EXIT, 1,
							"pbz2: *ERROR: Not enough resources to create fileWriter thread (code = %d)  Aborting...\n", ret);
					ret = pthread_join(TerminatorThread, NULL);
					return 1;
				}

				// start reading in data for decompression
				ret = producer_decompress(hInfile, InFileSize, fifo);
				if (ret == -99)
				{
					// only 1 block detected, use single threaded code to decompress
					noThreads = 1;

					switchedMtToSt = true;

					// wait for fileWriter thread to exit
					if (pthread_join(output, NULL) != 0)
					{
						ErrorContext::getInstance()->saveError();
						handle_error(EF_EXIT, 1,
								"pbz2: *ERROR: Error joining fileWriter thread (code = %d)  Aborting...\n", ret);
						errLevelCurrentFile = errLevel = 1;
						ret = pthread_join(TerminatorThread, NULL);
						return 1;
					}
				}
				else if (ret != 0)
				{
					errLevelCurrentFile = errLevel = 1;
				}
			}
			
			// use single threaded code
			if ((noThreads == 1) && (errLevelCurrentFile == 0))
			{
				if (QuietMode != 1)
					fprintf(stderr, "Decompressing data (no threads)...\n");

				if (hInfile > 0)
					close(hInfile);
				ret = directdecompress(InFilename, outFilename.c_str());
				if (ret != 0)
				{
					errLevelCurrentFile = errLevel = 1;
				}
			}
		} // if (decompress == 1)
		else
		{
			// do compression code
				
			// use multi-threaded code
			if (noThreads == 0)
			{
				if (QuietMode != 1)
					fprintf(stderr, "Compressing data...\n");
					
				for (i=0; (int)i < numCPU; i++)
				{
					ret = pthread_create(&fifo->consumers[i], &ChildThreadAttributes, consumer, fifo);
					if (ret != 0)
					{
						ErrorContext::getInstance()->saveError();
						handle_error(EF_EXIT, 1,
									 "pbz2: *ERROR: Not enough resources to create consumer thread #%u (code = %d)  Aborting...\n", i, ret);
						pthread_join(TerminatorThread, NULL);
						return 1;
					}
				}
	
				ret = pthread_create(&output, &ChildThreadAttributes, fileWriter, (void*)outFilename.c_str());
				if (ret != 0)
				{
					handle_error(EF_EXIT, 1,
							"pbz2: *ERROR: Not enough resources to create fileWriter thread (code = %d)  Aborting...\n", ret);
					pthread_join(TerminatorThread, NULL);
					return 1;
				}
	
				// start reading in data for compression
				ret = producer(hInfile, blockSize, fifo);
				if (ret != 0)
					errLevelCurrentFile = errLevel = 1;
			}
			else
			{
				// do not use threads for compression
				if (QuietMode != 1)
					fprintf(stderr, "Compressing data (no threads)...\n");

				ret = directcompress(hInfile, InFileSize, blockSize, outFilename.c_str());
				if (ret != 0)
					errLevelCurrentFile = errLevel = 1;
			}
		} // else

		if (noThreads == 0)
		{
			// wait for fileWriter thread to exit
			ret = pthread_join(output, NULL);
			if (ret != 0)
			{
				ErrorContext::printErrnoMsg(stderr, errno);
				errLevelCurrentFile = errLevel = 1;
			}
		}

		if ((noThreads == 0) || switchedMtToSt )
		{
			// wait for consumer threads to exit
			for (i = 0; (int)i < numCPU; i++)
			{
				ret = pthread_join(fifo->consumers[i], NULL);
				if (ret != 0)
				{
					ErrorContext::printErrnoMsg(stderr, errno);
					errLevelCurrentFile = errLevel = 1;
				}
			}
		}

		if (syncGetTerminateFlag() != 0)
		{
			errLevelCurrentFile = errLevel = 1;
		}

		if (OutputStdOut == 0)
		{
			// write store file meta data to output file
			if (writeFileMetaData(outFilename.c_str()) != 0)
			{
				handle_error(EF_NOQUIT, -1,
					"pbz2: *ERROR: Could not write file meta data to [%s]!\n", outFilename.c_str());
			}
		}

		// remove input file unless requested not to by user or error occurred
		if ( (keep != 1) && (errLevelCurrentFile == 0) )
		{
			struct stat statbuf;
			// only remove input file if output file exists
			bool removeFlag =
					(OutputStdOut != 0) ||
					(stat(outFilename.c_str(), &statbuf) == 0);

			if (removeFlag)
			{
				if (do_remove(InFilename) == -1)
				{
					handle_error(EF_NOQUIT, 1, "Can't remove input file [%s]!", InFilename);
				}
			}
		}

		// reclaim memory
		OutputBuffer.clear();
		fifo->clear();
		
		if ( (errLevelCurrentFile == 0) && (syncGetTerminateFlag() == 0) )
		{
			// finished processing file (mutex since accessed by cleanup procedure)
			safe_mutex_lock(&ErrorHandlerMutex);
			sigInFilename = NULL;
			sigOutFilename = NULL;
			safe_mutex_unlock(&ErrorHandlerMutex);
		}
		
		if (errLevelCurrentFile == 1)
		{
			syncSetTerminateFlag(1);
			break;
		}
		
		if (QuietMode != 1)
			fprintf(stderr, "-------------------------------------------\n");
	} /* for */

	// Explicit close on stdout if we've been writing there, after all input has been processed
	if (OutputStdOut == 1)
	{
		ret = close(STDOUT_FILENO);
		if (ret == -1)
		{
			ErrorContext::getInstance()->saveError();
			handle_error(EF_EXIT, 1, "pbz2: *ERROR: Failed to close STDOUT! Aborting...\n");
			exit(1);
		}
	}

	// Terminate signal handler thread sending SIGQUIT signal
	ret = pthread_kill(SignalHandlerThread, SIG_HANDLER_QUIT_SIGNAL);
	if (ret != 0)
	{
		fprintf(stderr, "Couldn't signal signal QUIT to SignalHandlerThread [%d]. Quitting prematurely!\n", ret);
		exit(errLevel);
	}
	else
	{
		ret = pthread_join(SignalHandlerThread, NULL);
		if (ret != 0)
		{
			fprintf(stderr, "Error on join of SignalHandlerThread [%d]\n", ret);
		}
	}

	if (syncGetTerminateFlag() == 0)
	{
		syncSetFinishedFlag(1);
	}
	
	ret = pthread_join(TerminatorThread, NULL);
	if (ret != 0)
	{
		fprintf(stderr, "Error on join of TerminatorThread [%d]\n", ret);
	}

	// reclaim memory
	queueDelete(fifo);
	mutexesDelete();
	disposeMemory(FileList);

	// get current time for end of benchmark
	#ifndef WIN32
	gettimeofday(&tvStopTime, &tz);
	#else
	GetSystemTime(&systemtime);
	SystemTimeToFileTime(&systemtime, (FILETIME *)&filetime);
	tvStopTime.tv_sec = filetime.QuadPart / 10000000;
	tvStopTime.tv_usec = (filetime.QuadPart - (LONGLONG)tvStopTime.tv_sec * 10000000) / 10;
	#endif

	#ifdef PBZIP_DEBUG
	fprintf(stderr, "\n Start Time: %ld + %ld\n", tvStartTime.tv_sec, tvStartTime.tv_usec);
	fprintf(stderr, " Stop Time : %ld + %ld\n", tvStopTime.tv_sec, tvStopTime.tv_usec);
	#endif

	// convert time structure to real numbers
	timeStart = (double)tvStartTime.tv_sec + ((double)tvStartTime.tv_usec / 1000000);
	timeStop = (double)tvStopTime.tv_sec + ((double)tvStopTime.tv_usec / 1000000);
	timeCalc = timeStop - timeStart;
	if (QuietMode != 1)
		fprintf(stderr, "\n     Wall Clock: %f seconds\n", timeCalc);



    for(int i = 0; i < args.argc; i ++)
    {
        free(args.argv[i]);
    }
    free(args.argv);

	exit(errLevel);
}

//============================================================================
//% Colton Koop
//% 301380869
//% ckoop (ckoop@sfu.ca)
//
// File Name   : myIO.cpp
// Version     : September 28, 2021
// Description : Wrapper I/O functions for ENSC-351
// Copyright (c) 2021 Craig Scratchley  (wcs AT sfu DOT ca)
//============================================================================

#include <unistd.h>			// for read/write/close
#include <fcntl.h>			// for open/creat
#include <sys/socket.h> 	// for socketpair
#include <stdarg.h>         // for va stuff
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <map>
#include <errno.h>
#include <termios.h>

#include "SocketReadcond.h"
#include "myIO.h"

class SocketInfo {
public:
    int pairedDes;
    int bytesOutstanding;
    bool wroteClosedFlag;
    std::condition_variable cv;
    std::mutex mut;
    SocketInfo(int pd){
        bytesOutstanding = 0;
        pairedDes = pd;
        wroteClosedFlag = 0;
    }
};

std::map<int, std::shared_ptr<SocketInfo>> m;
std::shared_mutex mapMut;

int myOpen(const char *pathname, int flags, ...) //, mode_t mode)
{
    mode_t mode = 0;
    // in theory we should check here whether mode is needed.
    va_list arg;
    va_start (arg, flags);
    mode = va_arg (arg, mode_t);
    va_end (arg);
	return open(pathname, flags, mode);
}

int myCreat(const char *pathname, mode_t mode)
{
	return creat(pathname, mode);
}

int mySocketpair( int domain, int type, int protocol, int des_array[2] )
{
    std::lock_guard<std::shared_mutex> lk(mapMut);
    int retVal = socketpair(domain, type, protocol, des_array);
    m.emplace(std::make_pair(des_array[0], std::make_shared<SocketInfo>(des_array[1])));
    m.emplace(std::make_pair(des_array[1], std::make_shared<SocketInfo>(des_array[0])));
    return retVal;
}

ssize_t myRead( int des, void* buf, size_t nbyte )
{
    int bytesRead;
    std::shared_lock<std::shared_mutex> maplk(mapMut);
    auto mapIter = m.find(des);

    if(mapIter != m.end()){//descriptor was found in the map
        std::shared_ptr<SocketInfo> thisSocket = mapIter->second;
        maplk.unlock();
        bytesRead = read(des, buf, nbyte);
        std::lock_guard<std::mutex> socklk(thisSocket->mut);
        thisSocket->bytesOutstanding -= bytesRead;
        thisSocket->cv.notify_all();
    }
    else{//descriptor not found in the map
        maplk.unlock();
        bytesRead = read(des, buf, nbyte);
    }

    return bytesRead;
}

ssize_t myWrite( int des, const void* buf, size_t nbyte )
{
    int bytesWritten;
    std::shared_lock<std::shared_mutex> maplk(mapMut);
    auto mapIter = m.find(des);

    if(mapIter != m.end()){//descriptor was found in the map
        std::shared_ptr<SocketInfo> thisSocket = mapIter->second;
        mapIter = m.find(thisSocket->pairedDes);
        if(mapIter != m.end()){//paired descriptor was found in the map
            std::shared_ptr<SocketInfo> pairedSocket = mapIter->second;
            maplk.unlock();
            bytesWritten = write(des, buf, nbyte);
            std::lock_guard<std::mutex> socklk(thisSocket->mut);
            pairedSocket->bytesOutstanding += bytesWritten;
        }
        else{
            errno = 32;//broken pipe
            return -1;
        }
    }
    else{//descriptor not found in the map
        maplk.unlock();
        bytesWritten = write(des, buf, nbyte);
    }

    return bytesWritten;
}

int myClose( int des )
{
    int saveThisBytesOutstanding = 0;
    std::lock_guard<std::shared_mutex> maplk(mapMut);
    auto mapIter = m.find(des);

    if(mapIter != m.end()){//descriptor was found in the map
        std::shared_ptr<SocketInfo> thisSocket = mapIter->second;
        mapIter = m.find(thisSocket->pairedDes);
        std::lock_guard<std::mutex> thisSocklk(thisSocket->mut);
        saveThisBytesOutstanding = thisSocket->bytesOutstanding;
        thisSocket->bytesOutstanding = 0;
        thisSocket->cv.notify_all();

        if(mapIter != m.end()){//paired descriptor was found in the map
            std::shared_ptr<SocketInfo> pairedSocket = mapIter->second;
            std::lock_guard<std::mutex> pairSocklk(pairedSocket->mut);
            if(saveThisBytesOutstanding > 0){
                pairedSocket->wroteClosedFlag = true;
            }
            pairedSocket->bytesOutstanding = 0;
            pairedSocket->cv.notify_all();
            pairedSocket->pairedDes = -1;
        }

        m.erase(m.find(des));
    }

    return close(des);
}

int myTcdrain( int des )
{
    std::shared_lock<std::shared_mutex> maplk(mapMut);
    auto mapIter = m.find(des);

    if(mapIter != m.end()){//descriptor was found in the map
        std::shared_ptr<SocketInfo> thisSocket = mapIter->second;
        mapIter = m.find(thisSocket->pairedDes);
        if(mapIter != m.end()){//paired descriptor was found in the map
            std::shared_ptr<SocketInfo> pairedSocket = mapIter->second;
            maplk.unlock();
            std::unique_lock<std::mutex> socklk(pairedSocket->mut);
            pairedSocket->cv.wait(socklk,[pairedSocket]{return pairedSocket->bytesOutstanding == 0;});
        }
        return 0;
    }

    return tcdrain(des);
}

/* Arguments:
des
    The file descriptor associated with the terminal device that you want to read from.
buf
    A pointer to a buffer into which readcond() can put the data.
n
    The maximum number of bytes to read.
min, time, timeout
    When used in RAW mode, these arguments override the behavior of the MIN and TIME members of the terminal's termios structure. For more information, see...
 *
 *  https://developer.blackberry.com/native/reference/core/com.qnx.doc.neutrino.lib_ref/topic/r/readcond.html
 *
 *  */
int myReadcond(int des, void* buf, int n, int min, int time, int timeout)
{
    int totalBytesRead = 0;
    int bytesJustRead = 0;
    std::shared_lock<std::shared_mutex> maplk(mapMut);
    auto mapIter = m.find(des);

    if(mapIter != m.end()){//descriptor was found in the map
        std::shared_ptr<SocketInfo> thisSocket = mapIter->second;
        maplk.unlock();
        if(min == 0){//there is no minimum so we can return if nothing is read
            thisSocket->cv.notify_all();
            totalBytesRead = wcsReadcond(des, buf, n, min, time, timeout);
            std::lock_guard<std::mutex> socklk(thisSocket->mut);
            thisSocket->bytesOutstanding -= bytesJustRead;
            thisSocket->cv.notify_all();
        }
        else{//there is a minimum so we need to keep trying until that minimum is reached
            while(totalBytesRead < min){//read what we can until the min is reached
                thisSocket->cv.notify_all();
                bytesJustRead = wcsReadcond(des, (char*)buf+totalBytesRead, (n-totalBytesRead), 1, time, timeout);
                if(bytesJustRead == -1){//descriptor must've closed
                    if(totalBytesRead > 0){
                        break;
                    }
                    else{
                        if(thisSocket->wroteClosedFlag){
                            thisSocket->wroteClosedFlag = false;
                            errno = 104;
                            return -1;
                        }
                        else{
                            break;
                        }
                    }
                }
                if(bytesJustRead == 0){ break; }//must've timed out then
                totalBytesRead += bytesJustRead;
                std::lock_guard<std::mutex> socklk(thisSocket->mut);
                thisSocket->bytesOutstanding -= bytesJustRead;
                thisSocket->cv.notify_all();
            }
        }
    }
    else{//descriptor not found in the map
        totalBytesRead = wcsReadcond(des, buf, n, min, time, timeout);
    }

    return totalBytesRead;
}

//assuming everything is hooked up correctly, probably done here

//============================================================================
//
//% Colton Koop
//% 301380869
//% ckoop (ckoop@sfu.ca)
//
// Version     : September, 2021
// Copyright   : Copyright 2021, Craig Scratchley
// Description : Starting point for ENSC 351 Project Part 2
//============================================================================

#include <stdlib.h> // EXIT_SUCCESS
#include <sys/socket.h>
#include <pthread.h>
#include <thread>

#include "myIO.h"
#include "Medium.h"

#include "VNPE.h"
#include "AtomicCOUT.h"
#include "posixThread.hpp"

#include "ReceiverY.h"
#include "SenderY.h"

using namespace std;
using namespace pthreadSupport;

enum  {Term1, Term2};
enum  {TermSkt, MediumSkt};

//static int daSktPr[2];	  //Socket Pair between term1 and term2
static int daSktPrT1M[2];	  //Socket Pair between term1 and medium
static int daSktPrMT2[2];	  //Socket Pair between medium and term2

void testReceiverY(int mediumD)
{
    COUT << "Will try to receive file(s) with CRC" << endl;
    ReceiverY yReceiverCRC(mediumD);
    yReceiverCRC.receiveFiles();
    COUT << "yReceiver result was: " << yReceiverCRC.result << endl  << endl;
}

void testSenderY(vector<const char*> iFileNames, int mediumD)
{
    SenderY ySender(iFileNames, mediumD);
    COUT << "test sending" << endl;
    ySender.sendFiles();
    COUT << "Sender finished with result: " << ySender.result << endl << endl;
}

// ***** modify this function to communicate with the "Kind Medium" *****
void termFunc(int termNum)
{
	if (termNum == Term1) {
        testReceiverY(daSktPrT1M[1]);        // file does not exist
        testReceiverY(daSktPrT1M[1]);        // empty file and normal file.

        pthreadSupport::setSchedPrio(20); // drop priority down somewhat.  FIFO?
        PE(myClose(daSktPrT1M[1]));
	}
	else { // Term2
		PE_0(pthread_setname_np(pthread_self(), "T2")); // give the thread (terminal 2) a name
	    // PE_0(pthread_setname_np("T2")); // Mac OS X

	    vector<const char*> iFileNamesA = {"/doesNotExist.txt"};
	    //vector<const char*> iFileNamesB = {"/home/osboxes/.sudo_as_admin_successful", "/home/osboxes/hs_err_pid11431.log"};
	    vector<const char*> iFileNamesB = {"/home/osboxes/.sudo_as_admin_successful", "/home/osboxes/hs_err_pid11431.log", "/home/osboxes/hs_err_pid11506.log"};

        testSenderY(iFileNamesA, daSktPrMT2[1]);   // file does not exist
        testSenderY(iFileNamesB, daSktPrMT2[1]);   // empty file and normal file.

        pthreadSupport::setSchedPrio(20); // drop priority down somewhat.  FIFO?
        PE(myClose(daSktPrMT2[1]));
	}
}

// ***** Modify this function to create the "Kind Medium" threads and communicate with it *****
int Ensc351Part2()
{
	PE_0(pthread_setname_np(pthread_self(), "P-T1")); // give the Primary thread (Terminal 1) a name
    // PE_0(pthread_setname_np("P-T1")); // Mac OS X

	// ***** switch from having one socketpair for direct connection to having two socketpairs
	//			for connection through medium threads *****
	PE(mySocketpair(AF_LOCAL, SOCK_STREAM, 0, daSktPrT1M));
	PE(mySocketpair(AF_LOCAL, SOCK_STREAM, 0, daSktPrMT2));
	//daSktPr[Term1] =  PE(/*myO*/open("/dev/ser2", O_RDWR));

    // ***** create thread with SCHED_FIFO priority 40 for medium *****
    //     have the thread run the function found in Medium.cpp:
    //          void mediumFunc(int T1d, int T2d, const char *fname)
    //          where T1d is the descriptor for the socket to Term1
    //          and T2d is the descriptor for the socket to Term2
    //          and fname is the name of the binary medium "log" file
    //          ("ymodemData.dat").
    //      Make sure that thread is created at SCHED_FIFO priority 40
    posixThread term2Thrd(SCHED_FIFO, 70, termFunc, Term2);
    posixThread mediumThrd(SCHED_FIFO, 40, mediumFunc, daSktPrT1M[0], daSktPrMT2[0],"ymodemData.dat");

	termFunc(Term1);

    term2Thrd.join();
    // ***** join with thread for medium *****
    mediumThrd.join();

	return EXIT_SUCCESS;
}

   # -*- coding:utf-8 -*-
#-----------------------------------------------------------------------------
#  Copyright (c) 2015, ROOT Team.
#  Authors: Danilo Piparo
#           Omar Zapata <Omar.Zapata@cern.ch> http://oproject.org
#  Distributed under the terms of the Modified LGPLv3 License.
#
#  The full license is in the file COPYING.rst, distributed with this software.
#-----------------------------------------------------------------------------
from ctypes import CDLL, c_char_p
from threading import Thread
from time import sleep as timeSleep
from resource import setrlimit, RLIMIT_STACK, RLIM_INFINITY
from sys import platform

_lib = CDLL("libJupyROOT.so")

class IOHandler(object):
    r'''Class used to capture output from C/C++ libraries.
    >>> import sys
    >>> h = IOHandler()
    >>> h.GetStdout()
    ''
    >>> h.GetStderr()
    ''
    >>> h.GetStreamsDicts()
    (None, None)
    >>> del h
    '''
    def __init__(self):
        for cfunc in [_lib.JupyROOTExecutorHandler_GetStdout,
                      _lib.JupyROOTExecutorHandler_GetStderr]:
           cfunc.restype = c_char_p
        _lib.JupyROOTExecutorHandler_Ctor()

    def __del__(self):
        _lib.JupyROOTExecutorHandler_Dtor()

    def Clear(self):
        _lib.JupyROOTExecutorHandler_Clear()

    def Poll(self):
        _lib.JupyROOTExecutorHandler_Poll()

    def InitCapture(self):
        _lib.JupyROOTExecutorHandler_InitCapture()

    def EndCapture(self):
        _lib.JupyROOTExecutorHandler_EndCapture()

    def GetStdout(self):
       return _lib.JupyROOTExecutorHandler_GetStdout()

    def GetStderr(self):
       return _lib.JupyROOTExecutorHandler_GetStderr()

    def GetStreamsDicts(self):
       out = self.GetStdout()
       err = self.GetStderr()
       outDict = {'name': 'stdout', 'text': out} if out != "" else None
       errDict = {'name': 'stderr', 'text': err} if err != "" else None
       return outDict,errDict

class Runner(object):
    ''' Asynchrously run functions
    >>> import time
    >>> def f(code):
    ...    print code
    >>> r= Runner(f)
    >>> r.Run("ss")
    ss
    >>> r.AsyncRun("ss");time.sleep(1)
    ss
    >>> def g(msg):
    ...    time.sleep(.5)
    ...    print msg
    >>> r= Runner(g)
    >>> r.AsyncRun("Asynchronous");print "Synchronous";time.sleep(1)
    Synchronous
    Asynchronous
    >>> r.AsyncRun("Asynchronous"); print r.HasFinished()
    False
    >>> time.sleep(1)
    Asynchronous
    >>> print r.HasFinished()
    True
    '''
    def __init__(self, function):
        self.function = function
        if platform != 'darwin':
            setrlimit(RLIMIT_STACK,(RLIM_INFINITY,RLIM_INFINITY))
        self.thread = None

    def Run(self, argument):
        return self.function(argument)

    def AsyncRun(self, argument):
        self.thread = Thread(target=self.Run, args =(argument,))
        self.thread.start()

    def Wait(self):
        if not self.thread: return
        self.thread.join()

    def HasFinished(self):
        if not self.thread: return True

        finished = not self.thread.is_alive()
        if not finished: return False

        self.thread.join()
        self.thread = None

        return True


class JupyROOTDeclarer(Runner):
    ''' Asynchrously execute declarations
    >>> import ROOT
    >>> d = JupyROOTDeclarer()
    >>> d.Run("int f(){return 3;}")
    1
    >>> ROOT.f()
    3
    '''
    def __init__(self):
       super(JupyROOTDeclarer, self).__init__(_lib.JupyROOTDeclarer)

class JupyROOTExecutor(Runner):
    r''' Asynchrously execute process lines
    >>> import ROOT
    >>> d = JupyROOTExecutor()
    >>> d.Run('cout << "Here am I" << endl;')
    1
    '''
    def __init__(self):
       super(JupyROOTExecutor, self).__init__(_lib.JupyROOTExecutor)

def RunAsyncAndPrint(executor, code, ioHandler, printFunction, silent = False, timeout = 0.1):
   ioHandler.Clear()
   ioHandler.InitCapture()
   executor.AsyncRun(code)
   while not executor.HasFinished():
         ioHandler.Poll()
         if not silent:
            printFunction(ioHandler)
            ioHandler.Clear()
         if executor.HasFinished(): break
         timeSleep(.1)
   executor.Wait()
   ioHandler.EndCapture()

#!/usr/bin/env python
import unittest
import xattr
import os
import errno
import random
import threading

mount_point = "/mnt/ceph"
folder = "vol"
quota = 300000000
objsize = 4194304

def syncWrite(path, size):
    try:
        f = open('/dev/zero', "r")
        out_fd = os.open(path, os.O_CREAT | os.O_SYNC | os.O_WRONLY)
        while size > objsize:
            data = f.read(objsize)
            os.write(out_fd, data)
            size -= objsize
        data = f.read(size)
        os.write(out_fd, data)
    finally:
        f.close()
        os.close(out_fd)


def asyncWrite(path, size):
    try:
        f = open('/dev/zero', "r")
        out_fd = os.open(path, os.O_CREAT | os.O_ASYNC | os.O_WRONLY)
        while size > objsize:
            data = f.read(objsize)
            os.write(out_fd, data)
            size -= objsize
        data = f.read(size)
        os.write(out_fd, data)
    finally:
        f.close()
        os.close(out_fd)


def calcSize(path):
    size = 0
    for root, dirs, files in os.walk(path):
        size += sum(os.path.getsize(os.path.join(root, name)) for name in files)
    return size


class SyncWriteThread(threading.Thread):
    def __init__(self, path, size):
        threading.Thread.__init__(self)
        self.path = path
        self.size = size

    def run(self):
        try:
            syncWrite(self.path, self.size)
        except OSError, err:
            if err.errno == errno.EDQUOT:
                print "out of quota"
            else:
                raise err


class AsyncWriteThread(threading.Thread):
    def __init__(self, path, size):
        threading.Thread.__init__(self)
        self.path = path
        self.size = size

    def run(self):
        try:
            asyncWrite(self.path, self.size)
        except OSError, err:
            if err.errno == errno.EDQUOT:
                print "out of quota"
            else:
                raise err


class TestSingleClient(unittest.TestCase):


    def setUp(self):
        if not os.path.ismount(mount_point):
            raise "%s is not a mount point" % mount_point
        os.system('rm -rf %s/*' % mount_point)
        self.folder_path = os.path.join(mount_point, folder)
        os.mkdir(self.folder_path)
        xattr.setxattr(self.folder_path, 'user.quota', str(quota))
        os.system('sync')


    def tearDown(self):
        pass


    def testSyncWrite(self):
        filesize = 41943040
        try:
            for i in range(quota/filesize + 1):
                syncWrite(os.path.join(self.folder_path, str(i)), filesize)
        except OSError, err:
            self.assertEqual(err.errno, errno.EDQUOT)

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize)

    def testAsyncWrite(self):
        filesize = 41943040
        try:
            for i in range(quota/filesize + 1):
                asyncWrite(os.path.join(self.folder_path, str(i)), filesize)
        except OSError, err:
            self.assertEqual(err.errno, errno.EDQUOT)

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize)


    def testAsyncWriteAndRemove(self):
        filesize = 41943040
        try:
            for i in range(2*(quota/filesize + 1)):
                path = os.path.join(self.folder_path, str(i))
                asyncWrite(path, filesize)
                if i % 2 == 0:
                    os.remove(path)
        except OSError, err:
            self.assertEqual(err.errno, errno.EDQUOT)

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize)


    def testAsyncWriteSmallFiles(self):
        filesize = 409600
        try:
            for i in range(quota/filesize + 1):
                asyncWrite(os.path.join(self.folder_path, str(i)), filesize)
        except OSError, err:
            self.assertEqual(err.errno, errno.EDQUOT)

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize)


    def testAsyncWriteInMultiLevels(self):
        filesize = 41943040
        levels = 10
        
        # make folders
        dirpath = self.folder_path
        for i in range(levels):
            dirpath = os.path.join(dirpath, "folder")
        os.makedirs(dirpath)
        
        try:
            random.seed()
            for i in range(quota/filesize + 1):
                level = random.randint(0, levels)
                path = self.folder_path
                for j in range(level):
                    path = os.path.join(path, "folder")
                path = os.path.join(path, str(i))
                asyncWrite(path, filesize)
        except OSError, err:
            self.assertEqual(err.errno, errno.EDQUOT)

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize)

    
    def testConcurrentSyncWrite(self):
        filesize = 41943040
        threads = list()
        
        for i in range(quota/filesize + 1):
            t = SyncWriteThread(os.path.join(self.folder_path, str(i)), filesize)
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize*len(threads))


    def testConcurrentAsyncWrite(self):
        filesize = 41943040
        threads = list()
        
        for i in range(quota/filesize + 1):
            t = AsyncWriteThread(os.path.join(self.folder_path, str(i)), filesize)
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize*len(threads))
    

    def testConcurrentAsyncWriteToOneFile(self):
        concurrency = 4
        filesize = [419430400, 200715200, 419430400, 100357600]
        threads = list()
        
        for i in range(concurrency):
            t = AsyncWriteThread(os.path.join(self.folder_path, '0'), filesize[i])
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize*len(threads))


    def testConcurrentAsyncWriteInMultiLevels(self):
        filesize = 41943040
        levels = 10
        threads = list()
        
        # make folders
        dirpath = self.folder_path
        for i in range(levels):
            dirpath = os.path.join(dirpath, "folder")
        os.makedirs(dirpath)
        
        random.seed()
        for i in range(quota/filesize + 1):
            level = random.randint(0, levels)
            path = self.folder_path
            for j in range(level):
                path = os.path.join(path, "folder")
            path = os.path.join(path, str(i))
            t = AsyncWriteThread(path, filesize)
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        size = calcSize(self.folder_path)
        self.assertTrue(abs(size - quota) < objsize*len(threads))

    
if __name__ == "__main__":
    #import sys;sys.argv = ['', 'Test.testName']
    unittest.main()

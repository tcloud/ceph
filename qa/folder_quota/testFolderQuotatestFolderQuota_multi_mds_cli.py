#!/usr/bin/env python2.6
import os
import sys
import syslog
import time
from optparse import OptionParser

if sys.getdefaultencoding() != 'utf-8':
    reload(sys)
    sys.setdefaultencoding('utf-8')



EXIT_CODE = {
    'EXIT_SUCCESS': os.EX_OK,
    'EXIT_UNKNOW': 1,
    'EXIT_ARGU': 2,
    'EXIT_SMALL_THAN_REAL': 3,
    'EXIT_SMALL_THAN_SUB': 4,
    'EXIT_TOO_LARGE': 5,
    'EXIT_COMMAND_NOT_FOUND': 127,
    }


class Usage(Exception):
    '''Usage class for argument error '''
    def __init__(self, msg):
        self.msg = msg

#CLIENTS = ['10.201.193.238', '10.201.193.240', '10.201.193.223']
g_clients = []
g_client_index = 0
#TMP_FULL_PATH = '/dd1/dd2/dd3/dd4/dd5/dd6/dd7/dd8/'
TMP_ROOT = '/dd1/'
TMP_EXPORT_DIR = 'dd4/' 
TMP_UPTREE = '%sdd2/dd3/' % TMP_ROOT
TMP_EXPORT_TREE = '%s%s' % (TMP_UPTREE, TMP_EXPORT_DIR)
TMP_SUBTREE = '%sdd5/dd6/dd7/dd8/' %TMP_EXPORT_DIR

TIMES_TOLLERANCE = 30 
SIZE_INC_BASE = 4 #MB 
QUOTA_SETTING = SIZE_INC_BASE * 4 

def subtree_test(root_path, monitor):
    '''
    T1: 
        set root quota
        export subtree to mds1
        Add file( size < quota ) in root, then add files to subtree until exceed
    T2: 
        set root quota
        export subtree to mds1
        Add file ( size < quota ) in subtree, theb add files to subtree until exceed
    T3:
        set root quota 
        export subtree to mds1
        Rotate add files to root & subtree until exceed
        increment root quota
        Rotate add files to root & subtree until exceed
    '''
    (iret, msg) = mount_root(root_path, monitor)
    if iret :
        return iret, msg
    # T1
    print '\n\nTest1: Add file( size < quota ) in root, then add files to subtree until exceed:'
    (iret, msg) = test_subtree_dedicated( root_path, monitor, 'exceed_on_subtree' )
    if iret :
        return iret, msg
    # T2
    print '\n\nTest2: Add file ( size < quota ) in subtree, theb add files to subtree until exceed:'
    (iret, msg) = test_subtree_dedicated( root_path, monitor, 'exceed_on_uptree' )
    if iret :
        return iret, msg
 
    #T3
    print '\n\nTest3: Rotate add files to root & subtree until exceed with 2 testing cycle:'
    (iret, msg) = test_subtree_mix(root_path, monitor, 2)
    if iret :
        return iret, msg

    return iret, msg
    
def test_subtree_mix(root_path, monitor, loop_times):
    iret = 0
    msg = ''
    (iret, msg) = prepare_subtree_env(root_path)
    if iret:
        return iret, msg

    quota_increment = QUOTA_SETTING
    quota = quota_increment
    inc_base = SIZE_INC_BASE
    times_tollerance = TIMES_TOLLERANCE
    total_size = 0

    for loop in range(0, loop_times):
        print '\nTesting for loop%d:' % loop
        #print 'cfolder_quota set -p %s%s -r %s -s %dM' % (
        #                root_path, TMP_ROOT, root_path, quota)
        (iret,msg) = rotate_client_cmd( 'cfolder_quota set -p %s%s -r %s -s %dM' % (
                        root_path, TMP_ROOT, root_path, quota), True)
        if iret:
            return iret, msg
        #os.system('sync')
        if ( loop == 0 ):
            print 'SSH %s: ceph mds tell 0 export_dir %s 1' % (monitor, TMP_EXPORT_TREE)
            (iret, msg) = execute_ssh(['ceph mds tell 0 export_dir %s 1' % TMP_EXPORT_TREE], monitor)
            if iret:
                return iret, msg
        
            #print 'mount -o remount %s' % root_path
            #(iret, msg) = rotate_client_cmd('mount -o remount %s' % root_path, False)
            #if ( iret != 0 ): 
            #    return iret, msg
            time.sleep(6)            
        
        times = quota / (loop + 1) / inc_base / 2 + times_tollerance
        stop = False
        for i in range(0,times):
            paths = ['%s%s' % (root_path, TMP_UPTREE), 
                     '%s%s%s' % (root_path, TMP_UPTREE, TMP_SUBTREE) ]
            for path in paths:
                #print 'dd if=/dev/zero of=%s/%dM bs=1M count=%d' % (
                #                    path, total_size, inc_base)
                (iret, msg) = rotate_client_cmd('dd if=/dev/zero of=%s/%d-%dM bs=1M count=%d' % (
                                    path, inc_base, total_size/inc_base, inc_base), True)                
                if iret:
                    #print msg
                    stop = True
                    break;
                else:
                    total_size += inc_base
            if stop == True:
                break;
        i += 1
        if i < times and i >= (times - times_tollerance):
            print 'Success for loop%d ---------------------\n' % loop
            iret = 0
        else:
            print 'Fail: (i=%d) quota=%d, total=%d' % (i, quota, total_size)   
            return -1, msg
        quota += quota_increment
            
    return iret, msg
           
        

def test_subtree_dedicated( root_path, monitor, exceed_on ):     
    iret = 0
    msg = ''
    (iret, msg) = prepare_subtree_env(root_path)
    if iret:
        return iret, msg
    quota = QUOTA_SETTING
    #print 'cfolder_quota set -p %s%s -r %s -s %dM' % (
    #                root_path, TMP_ROOT, root_path, quota)
    (iret,msg) = rotate_client_cmd('cfolder_quota set -p %s%s -r %s -s %dM' % (
                    root_path, TMP_ROOT, root_path, quota), False)
    if iret:
        return iret, msg
    #os.system('sync')

    print 'SSH %s: ceph mds tell 0 export_dir %s 1' % (monitor, TMP_EXPORT_TREE)
    (iret, msg) = execute_ssh(['ceph mds tell 0 export_dir %s 1' % TMP_EXPORT_TREE], monitor)
    if iret:
        return iret, msg

    #print 'mount -o remount %s' % root_path
    #(iret, msg) = rotate_client_cmd('mount -o remount %s' % root_path, False)
    #if ( iret != 0 ): 
    #    return iret, msg
    time.sleep(6)

    inc_base = SIZE_INC_BASE
    total_size = 0
    if exceed_on == 'exceed_on_subtree':
        path = '%s%s' % (root_path, TMP_UPTREE)
    else:
        path = '%s%s%s' % (root_path, TMP_UPTREE, TMP_SUBTREE)
    #print 'dd if=/dev/zero of=%s/%dM bs=1M count=%d' % (
    #                    path, total_size, total_size)
    (iret, msg) = rotate_client_cmd('dd if=/dev/zero of=%s/%d-%dM bs=1M count=%d' % (
                                    path, inc_base, total_size/inc_base, inc_base), True)
    if iret:
        return iret, msg
    if exceed_on == 'exceed_on_subtree':
        path = '%s%s%s' % (root_path, TMP_UPTREE, TMP_SUBTREE)
    else:
        path = '%s%s' % (root_path, TMP_UPTREE)
    total_size += inc_base
    (iret, msg) = cp_dedicated(path, quota, total_size, inc_base, TIMES_TOLLERANCE )
    if iret:
        return iret, msg

    return iret, msg


  
def cp_dedicated (path, quota, current_size, inc_base, times_tollerance):
    iret = 0
    msg =''
    total_size = current_size
    times = (quota - current_size)/inc_base + times_tollerance
    for i in range(0, times):
        #print 'dd if=/dev/zero of=%s/%dM bs=1M count=%d' % (
        #                    path, total_size, inc_base)
        (iret, msg) = rotate_client_cmd('dd if=/dev/zero of=%s/%d-%dM bs=1M count=%d' % (
                                    path, inc_base, total_size/inc_base, inc_base), True)
        if iret:
            #print msg
            break;
        total_size += inc_base
        
    i += 1
    if i < times and i >= (times - times_tollerance):
        print 'Success'
        return 0, msg
    else:
        print 'Fail: (i=%d) quota=%d, total=%d' % (i, quota, total_size)     
        return -1, msg
    return iret, msg
    
def mount_root(root_path, monitor):
    iret = 0
    msg = ''
    #print 'umount %s' % root_path    
    (iret, msg) = multi_client_cmd('umount %s' % root_path, False, True)
    #print 'mount -t ceph %s:/ %s -o folder_quota=1' % (monitor, root_path)
    (iret, msg) = multi_client_cmd('mount -t ceph %s:/ %s -o folder_quota=1' % (
                                    monitor, root_path), True, True)
    if ( iret != 0 ): 
        return iret, msg
    return iret, msg
        
    
def prepare_subtree_env( root_path ):
    iret = 0
    msg =''
    #print 'rm -rf %s%s' % (root_path, TMP_ROOT)
    (iret, msg) = rotate_client_cmd('rm -rf %s%s' % (root_path, TMP_ROOT), False)
    if ( iret != 0 ): 
        return iret, msg
    #print 'mkdir -p %s%s%s' % (root_path, TMP_UPTREE, TMP_SUBTREE)
    (iret, msg) = rotate_client_cmd('mkdir -p %s%s%s' % (root_path, TMP_UPTREE, TMP_SUBTREE), False)
    if ( iret != 0 ): 
        return iret, msg
    #print 'mount -o remount %s' % root_path
    #(iret, msg) = rotate_client_cmd('mount -o remount %s' % root_path, False)
    #if ( iret != 0 ): 
    #    return iret, msg
    return iret, msg

def rotate_client_cmd( remote_cmd, next_client ):
    global g_clients
    global g_client_index

    if g_clients == []:
        print '%s ' % (remote_cmd)        
        (iret, lines) = execute_cmd([ remote_cmd ])
        if iret != os.EX_OK:
            print lines
    else:
        if next_client == True:
            g_client_index += 1
        if ( g_client_index >= len(g_clients) ):
            g_client_index = 0
    
        print 'SSH %s: %s ' % (g_clients[g_client_index], remote_cmd)
        (iret, lines) = execute_ssh([ remote_cmd ], g_clients[g_client_index], 'root')
        if iret != os.EX_OK:
            print lines
    
    #time.sleep(1)
    return iret, lines


def multi_client_cmd( remote_cmd, fail_stop, show_msg ):
    global g_clients
    if g_clients == []:
        (iret, lines) = execute_cmd([ remote_cmd ])
        print '%s ' % (remote_cmd)
        if show_msg == True and len(lines) != 0:
            print lines
        #check return code, if fail, break and go_out=True        
        if iret != os.EX_OK and fail_stop == True:
            print 'Stop: CMD(%s) ON %s' % (remote_cmd, machine)
    else :
        for machine in g_clients:
            (iret, lines) = execute_ssh([ remote_cmd ], machine, 'root')
            print 'SSH  %s: %s ' % (machine, remote_cmd)
            if show_msg == True and len(lines) != 0:
                print lines
            #check return code, if fail, break and go_out=True        
            if iret != os.EX_OK and fail_stop == True:
                print 'Stop: CMD(%s) ON %s' % (remote_cmd, machine)
   
        
    #time.sleep(1)
    return iret, lines

g_deep_layer = 0
def mkfolder(path, sub_name, level):
    path_new = '%s/%s' %( path, sub_name)
    if level < g_deep_layer:
        sub_name_next='%s-1' % sub_name
        mkfolder(path_new, sub_name_next, level+1)
        sub_name_next='%s-2' % sub_name
        mkfolder(path_new, sub_name_next, level+1)
        sub_name_next='%s-3' % sub_name
        mkfolder(path_new, sub_name_next, level+1)
    else: 
        cmd = 'mkdir -p %s' %( path_new)
        print cmd
        os.system(cmd)
        os.system('echo "%s" >> %s/f_%s.txt' % (path_new, path_new, sub_name))

def create_multi_folder( root_path ):
    global g_deep_layer
    width = 3
    g_deep_layer = 5
    for i in range(0,width):
        mkfolder(root_path, "d1", 0);
        '''
        path = root_path + "/" + str(i) + "/"
        os.system("mkdir -p %s" % path)
        for j in range(0,deep):
            path += str(i) + "_" + str(j) + "/"
        os.system("mkdir -p %s" % path)
        print "mkdir -p %s" % path
        os.system('echo "%s" >> %s/%d_%d_f.txt' % (path, path, width, deep))
        '''
    return 0,""

def create_deep_folder(folder_path, deep_layer):
    path = folder_path + "/"
    for i in range(0,deep_layer):
        path += "d_" + str(i) + "/"
    os.system("mkdir -p %s" %path)    
    print "mkdir -p %s" %path
    os.system('echo "%s" >> %s/f_path_%d.txt' % (path, path,deep_layer))
    return 0,""

def main(argv=None):
    ''' Main entry for this API. '''
    if argv is None:
        argv = sys.argv

    usage = '''%prog --help for options details.

Command:
    cmd -c remote_cmd
    create -r root_path
    create_deep -p folder_path -l deep_layer
    subtree -r root_path -m monitor [ -C "['IP1','IP2'...]" ]
'''
    parser = OptionParser(usage)
    parser.add_option('-p', '--path', dest = 'folder_path', 
        help = 'The folder path')
    parser.add_option('-r', '--root', dest = 'root_path', 
        help = 'The root mount path')
    parser.add_option('-c', '--cmd', dest = 'cmd',   
        help = 'The cmd exe on remote client')
    parser.add_option('-l', '--deep_layer', dest = 'deep_layer', type = 'int',   
        help = 'deep layer')
    parser.add_option('-m', '--monitor', dest = 'monitor',    
        help = 'The cmd exe on remote client')
    parser.add_option('-C', '--Clients', dest = 'clients',    
        help = 'The remote clients')
    (options, args) = parser.parse_args(argv)
    
    iret = EXIT_CODE['EXIT_SUCCESS']
    rmsg = ''
    
    try:
        if len(args) < 2:
            raise Usage("Please specify Command to run")
        
        global g_clients
        if options.clients != None:
            g_clients = eval(options.clients)
    
        command = args[1]
        if command == 'cp':
            if  options.root_path == None:
                raise Usage('You must specify the mount path of root folder by -r') 
            (iret, rmsg) = multi_client_test( options.root_path) 
        elif command == 'cmd':
            if options.cmd == None or g_clients == []:
                raise Usage('You must specify the remote cmd by -c cmd -C [\'IP1\',\'IP2\'...]')            
            (iret, rmsg) = multi_client_cmd( options.cmd, True, True)
        elif command == 'create':
            if options.root_path == None:
                raise Usage('You must specify the root path by -r root_path')
            (iret, rmsg) = create_multi_folder( options.root_path )
        elif command == 'create_deep':
            if options.folder_path == None or options.deep_layer == None:
                raise Usage('You must specify the folder path by -p folder_path and deep layer by -l deep_layer')
            (iret, rmsg) = create_deep_folder( options.folder_path, options.deep_layer )
        elif command == 'subtree':
            if options.root_path == None or options.monitor == None:
                raise Usage('You must specify the root mount path by -r, monitor by -m [Clients by -C "[\'IP1\',\'IP2\'...]"]')
            (iret, rmsg) = subtree_test( options.root_path, options.monitor )
    except Usage, err:
        print >> sys.stderr, err.msg
        print >> sys.stderr, "for help, use command %s --help" % sys.argv[0]
        syslog.syslog('%s: %s' % (sys.argv[0], str(err.msg)))
        iret = EXIT_CODE['EXIT_ARGU']
        
    except:
        iret = EXIT_CODE['EXIT_UNKNOW']
        raise

    if iret != EXIT_CODE['EXIT_SUCCESS'] and rmsg != '':
        print >> sys.stderr, rmsg
        syslog.syslog('%s: %s' %(sys.argv[0], rmsg))

    return iret
        

import subprocess
class ExecuteError(Exception):
    """
    This Exception will be raised when the subprocess doesn't exit
    """
    def __init__(self, msg):
        """
        Constructor, set the exception message

        @type   msg:    string
        @param  msg:    The error message
        """
        Exception.__init__(self)
        self.msg = msg

    def __str__(self):
        """
        For print statement

        @rtype:     string
        @return:    Error message for the exception
        """
        return repr(self.msg)

def execute_cmd(cmd_lists):
    """
    Execution helper for command
    
    @type   cmd_lists:      list
    @param  cmd_lists:      The commmand list to execute
    @rtype:                 tuple
    @return:                (rc, lines): return code and output lines. If 
                            rc is os.EX_OK, lines will be from 
                            stdout.readlines(), otherwise, it will be from 
                            stderr.readlines()

    @raise ExecuteError:    If the process cannot execute, the ExecuteError will
                            be raised
    """

    bash_cmd = '&&'.join(cmd_lists)
    cmd = 'bash -c "%s"' % (bash_cmd)

    try:
        proc = subprocess.Popen(args=cmd, shell=True, 
                stdout=subprocess.PIPE)
        sts = os.waitpid(proc.pid, 0)[1]
    except (OSError, IOError):
        # Perform Exception Chaining in Python 2.x
        exc, trackback = sys.exc_info()[1:]
        new_exc = ExecuteError(str(exc))
        raise new_exc.__class__, new_exc, trackback
        
    if os.WIFEXITED(sts):
        return_code = os.WEXITSTATUS(sts)
    else:
        err_msg = "Fail to execute cmd: (%s): %d" % (cmd, sts[1])
        raise ExecuteError(err_msg)

    if return_code == os.EX_OK:
        lines = proc.stdout.readlines()
    else:
        lines = proc.stdout.readlines()

    return (return_code, lines)

def execute_ssh(cmd_lists, remote_host, remote_user='root', id_file=None):
    """
    Execution helper for remote ssh command

    @type   cmd_lists:      list
    @param  cmd_lists:      The command list to execute
    @type   remote_host:    string
    @param  remote_host:    Remote SSH hostname
    @type   remote_user:    string
    @param  remote_user:    Remote SSH user
    @type   id_file:        string
    @param  id_file:        Path of SSH private key

    @rtype:                 tuple
    @return:                (rc, lines): return code and output lines. If 
                            rc is os.EX_OK, lines will be from 
                            stdout.readlines(), otherwise, it will be from 
                            stderr.readlines()

    @raise ValueError:      The given parameters are inappropriate values 
    @raise ExecuteError:    If the process cannot exit, the ExecuteError will
                            be raised
    """

    # Remote hostname checking
    if not remote_host:
        raise ValueError('Please specify remote host IP or Hostname')

    # Remote username checking
    if not remote_user:
        raise ValueError('Please specify remote user name')

    ssh_cmd = ['ssh']

    if id_file:
        if not os.access(id_file, os.R_OK):
            err_msg = 'Please specify correct identity file (%s)' % id_file
            raise ValueError(err_msg)
        
        # Add SSH private key by "-i <key_file>"
        ssh_cmd += ['-i', id_file]
    
    # Add ssh_user@ssh_host 
    ssh_cmd.append('%s@%s' % (remote_user, remote_host))

    bash_cmd = ' && '.join(cmd_lists)
    cmd_args = ssh_cmd + ['/bin/bash', '-c', '"%s"' % bash_cmd]
   
    try: 
        proc = subprocess.Popen(args=cmd_args, 
                stdout=subprocess.PIPE, 
                stderr=subprocess.PIPE)
        sts = os.waitpid(proc.pid, 0)[1]
    except (OSError, IOError):
        # Perform Exception Chaining in Python 2.x
        exc, trackback = sys.exc_info()[1:]
        new_exc = ExecuteError(str(exc))
        raise new_exc.__class__, new_exc, trackback
 
    if os.WIFEXITED(sts):
        return_code = os.WEXITSTATUS(sts)
    else:
        err_msg = "Fail to execute cmd: (%s): %d" % (cmd_args, sts[1])
        raise ExecuteError(err_msg)

    if return_code == os.EX_OK:
        lines = proc.stdout.readlines()
    else:
        lines = proc.stderr.readlines()

    return (return_code, lines)


if __name__ == '__main__':
    sys.exit(main())

# vim: noautoindent tabstop=4 expandtab shiftwidth=4 softtabstop=4


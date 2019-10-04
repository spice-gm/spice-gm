#!/usr/bin/env python
"""
Spice Migration test

Somewhat stressfull test of continuous migration with spice in VGA mode or QXL mode,
depends on supplying an image in IMAGE variable (if no image is supplied then
VGA mode since it will just be SeaBIOS).

Dependencies:
either qmp in python path or running with spice and qemu side by side:
qemu/python/qemu/qmp.py
spice/tests/migrate.py

Will create two temporary unix sockets in /tmp
Will leave a log file, migrate_test.log, in current directory.
"""

#
# start one spice client, have two machines (active and target),
# and repeat:
#  active wait until it's active
#  active client_migrate_info
#  active migrate tcp:$hostname:9000
#  _wait for event of quit
#  active stop, active<->passive
#
# wait until it's active
#  command query-status, if running good
#  if not listen to events until event of running

try:
    import qmp
except:
    import sys
    sys.path.append("../../qemu/python/qemu/")
    try:
        import qmp
    except:
        print("can't find qmp")
        raise SystemExit
import sys
from subprocess import Popen, PIPE
import os
import time
import socket
import datetime
import atexit
import argparse

# python3 does not have raw_input
if sys.version_info[0] == 3:
    raw_input = input

def run_shell_command(cmd):
    return os.popen(cmd).read().strip()

def get_args():
    parser = argparse.ArgumentParser(description='Process some integers.')
    parser.add_argument('--qmp1', dest='qmp1', default='/tmp/migrate_test.1.qmp')
    parser.add_argument('--qmp2', dest='qmp2', default='/tmp/migrate_test.2.qmp')
    parser.add_argument('--spice_port1', dest='spice_port1', type=int, default=5911)
    parser.add_argument('--spice_port2', dest='spice_port2', type=int, default=6911)
    parser.add_argument('--migrate_port', dest='migrate_port', type=int, default=8000)
    parser.add_argument('--qemu', dest='qemu', default='../../qemu/x86_64-softmmu/qemu-system-x86_64')
    parser.add_argument('--log_filename', dest='log_filename', default='migrate.log')
    parser.add_argument('--image', dest='image', default='')
    parser.add_argument("--hostname", dest='hostname', default='localhost',
                        help="Set hostname used in migration message (default: localhost")
    parser.add_argument('--client', dest='client', default='none', choices=['spicy', 'remote-viewer', 'none'],
                        help="Automatically lunch one of supported clients or none (default)")
    parser.add_argument('--vdagent', dest="vdagent", action='store_true', default=False,
                        help="Append options for agent's virtserialport")
    parser.add_argument('--wait-user-input', dest="wait_user_input", action='store_true', default=False,
                        help="Wait user's input to start migration test")
    parser.add_argument('--wait-user-connect', dest="wait_user_connect", action='store_true', default=False,
                        help="Wait spice client to connect to move to next step of migration (default False)")
    parser.add_argument('--count', dest='counter', type=int, default=100,
                        help="Number of migrations to run (set 0 for infinite)")
    args = parser.parse_args(sys.argv[1:])
    if os.path.exists(args.qemu):
        args.qemu_exec = args.qemu
    else:
        args.qemu_exec = run_shell_command("which %s" % args.qemu)
    if not os.path.exists(args.qemu_exec):
        print("qemu not found (qemu = %r)" % args.qemu_exec)
        sys.exit(1)
    return args

def start_qemu(qemu_exec, image, spice_port, qmp_filename, incoming_port=None, with_agent=False):
    args = [
        qemu_exec,
        "-qmp", f"unix:{qmp_filename},server,nowait",
        "-spice", f"disable-ticketing,port={spice_port}"
    ]
    if incoming_port:
        args += (f"-incoming tcp::{incoming_port}").split()

    if with_agent:
        args += [
            '-device', 'virtio-serial',
            '-chardev', 'spicevmc,name=vdagent,id=vdagent',
            '-device', 'virtserialport,chardev=vdagent,name=com.redhat.spice.0'
        ]

    if os.path.exists(image):
        args += [
            "-m", "512",
            "-enable-kvm",
            "-drive", f"file={image},index=0,media=disk,cache=writeback"
        ]

    proc = Popen(args, executable=qemu_exec, stdin=PIPE, stdout=PIPE)

    while not os.path.exists(qmp_filename):
        time.sleep(0.1)
    proc.qmp_filename = qmp_filename
    proc.qmp = qmp.QEMUMonitorProtocol(qmp_filename)
    while True:
        try:
            proc.qmp.connect()
            break
        except socket.error:
            pass
    proc.spice_port = spice_port
    proc.incoming_port = incoming_port
    return proc

def start_client(client, hostname, spice_port):
    client_cmd = f"spicy --uri spice://{hostname}:{spice_port}"
    if client == "remote-viewer":
        client_cmd = f"remote-viewer spice://{hostname}:{spice_port}"

    return Popen(client_cmd.split(), executable=client)

def wait_active(q, active):
    while True:
        try:
            ret = q.cmd("query-status")
            if ret["return"]["running"] == active:
                break
        except:
            # ValueError
            time.sleep(0.1)
            continue

        time.sleep(0.5)

def wait_for_event(q, event):
    while True:
        for e in q.get_events():
            if e["event"] == event:
                return
        time.sleep(0.5)

def cleanup(migrator):
    print("doing cleanup")
    migrator.close()

def remove_image_file(filename):
    run_shell_command('rm -f %s' % filename)

class Migrator(object):

    migration_count = 0

    def __init__(self, log, client, qemu_exec, image, monitor_files,
                 spice_ports, migration_port, vdagent, hostname):
        self.client = client if client != "none" else None
        self.log = log
        self.qemu_exec = qemu_exec
        self.image = image
        self.migration_port = migration_port
        self.monitor_files = monitor_files
        self.spice_ports = spice_ports
        self.vdagent = vdagent
        self.hostname = hostname

        self.active = start_qemu(qemu_exec=qemu_exec,
                                 image=image,
                                 spice_port=spice_ports[0],
                                 qmp_filename=monitor_files[0],
                                 with_agent=self.vdagent)
        self.target = start_qemu(qemu_exec=qemu_exec,
                                 image=image,
                                 spice_port=spice_ports[1],
                                 qmp_filename=monitor_files[1],
                                 with_agent=self.vdagent,
                                 incoming_port=migration_port)
        self.remove_monitor_files()
        self.connected_client = None

    def close(self):
        self.remove_monitor_files()
        self.kill_qemu()

    def kill_qemu(self):
        for p in [self.active, self.target]:
            print("killing and waiting for qemu pid %s" % p.pid)
            p.kill()
            p.wait()

    def remove_monitor_files(self):
        for x in self.monitor_files:
            if os.path.exists(x):
                os.unlink(x)

    def iterate(self, wait_for_user_input=False, wait_user_connect=False):
        wait_active(self.active.qmp, True)
        wait_active(self.target.qmp, False)
        if not self.connected_client:
            if self.client:
                self.connected_client = start_client(client=self.client,
                                                     hostname=self.hostname,
                                                     spice_port=self.spice_ports[0])

            if wait_for_user_input:
                print("waiting for Enter to start migrations")
                raw_input()

        # Tester can launch its own client or we wait start_client() to connect
        if self.connected_client or wait_user_connect:
            wait_for_event(self.active.qmp, 'SPICE_INITIALIZED')

        self.active.qmp.cmd('client_migrate_info', {
                                'protocol' : 'spice',
                                'hostname' : self.hostname,
                                'port' : self.target.spice_port
                            })
        self.active.qmp.cmd('migrate', {
                                'uri': f'tcp:localhost:self.migration_port',
                                'uri': f'tcp:{self.hostname}:{self.migration_port}'
                            })
        wait_active(self.active.qmp, False)
        wait_active(self.target.qmp, True)

        if self.connected_client or wait_user_connect:
            wait_for_event(self.target.qmp, 'SPICE_CONNECTED')

        dead = self.active
        dead.qmp.cmd("quit")
        dead.qmp.close()
        dead.wait()
        new_spice_port = dead.spice_port
        new_qmp_filename = dead.qmp_filename

        outstr = dead.stdout.read()
        if outstr:
            outstr = outstr.decode(encoding='utf-8', errors='ignore')
            self.log.write("# STDOUT dead %s\n" % dead.pid)
            self.log.write(outstr)

        del dead
        self.active = self.target
        self.target = start_qemu(spice_port=new_spice_port,
                                 qemu_exec=self.qemu_exec,
                                 image=self.image,
                                 qmp_filename=new_qmp_filename,
                                 with_agent=self.vdagent,
                                 incoming_port=self.migration_port)
        self.migration_count += 1
        print(self.migration_count)

def main():
    args = get_args()
    print("log file %s" % args.log_filename)
    log = open(args.log_filename, "a+")
    log.write("# "+str(datetime.datetime.now())+"\n")
    newimage = run_shell_command("mktemp --dry-run /tmp/migrate_XXXXXX.qcow2")
    qemu_img = run_shell_command("dirname %s" % args.qemu_exec) + '/qemu-img'
    run_shell_command('%s create -f qcow2 -b %s %s' % (qemu_img, args.image, newimage))
    print('using new image %s' % newimage)
    migrator = Migrator(client=args.client,
                        qemu_exec=args.qemu_exec,
                        image=newimage,
                        log=log,
                        monitor_files=[args.qmp1, args.qmp2],
                        migration_port=args.migrate_port,
                        spice_ports=[args.spice_port1, args.spice_port2],
                        vdagent=args.vdagent,
                        hostname=args.hostname)
    atexit.register(cleanup, migrator)
    atexit.register(remove_image_file, newimage)
    counter = 0
    while args.counter == 0 or counter < args.counter:
        migrator.iterate(wait_for_user_input=args.wait_user_input,
                         wait_user_connect=args.wait_user_connect)
        counter += 1

if __name__ == '__main__':
    main()

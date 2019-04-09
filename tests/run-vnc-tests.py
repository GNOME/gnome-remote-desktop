#!/usr/bin/python3

import dbus
import os
import time
import os.path
import subprocess
import sys

from gi.repository import GLib

from dbus.mainloop.glib import DBusGMainLoop

mutter = None

DBusGMainLoop(set_as_default=True)
loop = GLib.MainLoop()
bus = dbus.SessionBus()

vnc_client_failed = None
vnc_server_failed = None

os.environ['GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD'] = 'secret'
os.environ['MUTTER_DEBUG_DUMMY_MODE_SPECS'] = '1024x768'

def run_vnc_test_client():
  print("Running VNC test client")
  global vnc_client_failed
  builddir=os.getenv("TEST_BUILDDIR")
  vnc_test_client_path = os.path.join(builddir, 'tests', 'test-client-vnc')
  vnc_test_client = subprocess.Popen([vnc_test_client_path, 'localhost:5912'],
                                     stderr=subprocess.STDOUT)
  vnc_test_client.wait()
  if vnc_test_client.wait() != 0:
    print("VNC test client exited incorrectly")
    vnc_client_failed = True
  else:
    vnc_client_failed = False

def start_vnc_server():
  print("Starting VNC server")
  global vnc_server
  builddir=os.getenv("TEST_BUILDDIR")
  vnc_server_path = os.path.join(builddir, 'src', 'gnome-remote-desktop-daemon')
  vnc_server = subprocess.Popen([vnc_server_path, '--vnc-port', '5912'],
                                stderr=subprocess.STDOUT)
  time.sleep(1)

def stop_vnc_server():
  print("Stopping VNC server")
  global vnc_server
  global vnc_server_failed
  vnc_server.terminate()
  vnc_server.wait()
  if vnc_server.returncode != -15 and vnc_server.returncode != 0:
    print("VNC server exited incorrectly: %d"%(vnc_server.returncode))
    vnc_server_failed = True
  else:
    vnc_server_failed = False

def remote_desktop_name_appeared_cb(name):
  if name == '':
    return
  print("Remote desktop capable display server appeared")
  start_vnc_server()
  run_vnc_test_client()
  stop_vnc_server()
  stop_mutter()

def start_mutter():
  global mutter
  print("Starting mutter")
  mutter = subprocess.Popen(['mutter', '--nested', '--wayland'],
                            stderr=subprocess.STDOUT)

def stop_mutter():
  global mutter
  global loop
  if mutter == None:
    print("no mutter")
    return
  print("Stopping mutter")
  mutter.terminate()
  print("Waiting for mutter to terminate")
  if mutter.wait() != 0:
    print("Mutter exited incorrectly")
    sys.exit(1)
  print("Done")
  loop.quit()

bus.watch_name_owner('org.gnome.Mutter.RemoteDesktop',
                     remote_desktop_name_appeared_cb)

start_mutter()

loop.run()

if vnc_server_failed != False or vnc_client_failed != False:
  sys.exit(1)
else:
  sys.exit(0)

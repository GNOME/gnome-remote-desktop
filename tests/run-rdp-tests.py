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

rdp_client_failed = None
rdp_server_failed = None

os.environ['GNOME_REMOTE_DESKTOP_TEST_RDP_USERNAME'] = 'TestU'
os.environ['GNOME_REMOTE_DESKTOP_TEST_RDP_PASSWORD'] = 'TestPw'

def run_rdp_test_client():
  print("Running RDP test client")
  global rdp_client_failed
  builddir=os.getenv("TEST_BUILDDIR")
  rdp_test_client_path = os.path.join(builddir, 'tests', 'test-client-rdp')
  rdp_test_client = subprocess.Popen([rdp_test_client_path, '/v:127.0.0.1:3395', '/u:TestU', '/p:TestPw'],
                                     stderr=subprocess.STDOUT)
  rdp_test_client.wait()
  if rdp_test_client.returncode != 0:
    print("RDP test client exited incorrectly: %d"%(rdp_test_client.returncode))
    rdp_client_failed = True
  else:
    rdp_client_failed = False

def start_rdp_server():
  print("Starting RDP server")
  global rdp_server
  builddir=os.getenv("TEST_BUILDDIR")
  rdp_server_path = os.path.join(builddir, 'src', 'gnome-remote-desktop-daemon')
  rdp_server = subprocess.Popen([rdp_server_path, '--rdp-port', '3395'],
                                stderr=subprocess.STDOUT)
  time.sleep(5)

def stop_rdp_server():
  print("Stopping RDP server")
  global rdp_server
  global rdp_server_failed
  rdp_server.terminate()
  rdp_server.wait()
  if rdp_server.returncode != -15 and rdp_server.returncode != 0:
    print("RDP server exited incorrectly: %d"%(rdp_server.returncode))
    rdp_server_failed = True
  else:
    rdp_server_failed = False

def remote_desktop_name_appeared_cb(name):
  if name == '':
    return
  print("Remote desktop capable display server appeared")
  start_rdp_server()
  run_rdp_test_client()
  stop_rdp_server()
  stop_mutter()

def start_mutter():
  global mutter
  print("Starting mutter")
  mutter_path = os.getenv('MUTTER_BIN', 'mutter')
  mutter = subprocess.Popen([mutter_path, '--headless', '--wayland', '--no-x11'],
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

if rdp_server_failed != False or rdp_client_failed != False:
  sys.exit(1)
else:
  sys.exit(0)

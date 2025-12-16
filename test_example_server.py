
import subprocess
import socket
import time
import os
import signal
import sys

PORTS = [5001, 5002, 5003]
CLIENT_PORTS = [6001, 6002, 6003]
HOST = '127.0.0.1'

def clean_wal():
    for port in PORTS:
        try:
            os.remove(f"wal_{port}.log")
        except FileNotFoundError:
            pass

def start_server(id, port, peers):
    cmd = ["./example_server", str(id), str(port)]
    for p in peers:
        cmd.append(f"{HOST}:{p}")
    server_out = open(f"server_{port}.out", "w")
    return subprocess.Popen(cmd, stdout=server_out, stderr=server_out)

def send_command(port, cmd):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect((HOST, port))
        s.sendall((cmd + "\n").encode())
        data = s.recv(1024).decode().strip()
        s.close()
        return data
    except Exception as e:
        return f"ERROR: {e}"

def run_test():
    print("Cleaning WAL logs...")
    clean_wal()

    print("Starting 3 replicas...")
    servers = []
    # Server 1
    servers.append(start_server(PORTS[0], PORTS[0], [PORTS[1], PORTS[2]]))
    # Server 2
    servers.append(start_server(PORTS[1], PORTS[1], [PORTS[0], PORTS[2]]))
    # Server 3
    servers.append(start_server(PORTS[2], PORTS[2], [PORTS[0], PORTS[1]]))

    print("Waiting for leader election (5s)...")
    time.sleep(5)

    print("Writing values...")
    # Try writing to different nodes to verify redirect/forwarding.
    # The server should forward 'set' requests to the leader via multiplexed peer connection.

    # Try writing to all nodes - forwarding should make this succeed even if not leader
    for port in CLIENT_PORTS:
        print(f"Trying set on {port}...")
        resp = send_command(port, f"set foo{port} bar{port}")
        if resp == "OK":
            print(f"Set foo{port}=bar{port} on {port} SUCCEEDED (Direct or Forwarded)")
        else:
            print(f"Failed to set on {port}: {resp}")
            return 1 # We expect success on all due to forwarding



    time.sleep(1) # Wait for commit

    print("Reading values (verification)...")
    success = True
    for port in CLIENT_PORTS:
        # Verify all writes
        for write_port in CLIENT_PORTS:
             val = send_command(port, f"get foo{write_port}")
             if val != f"bar{write_port}":
                  print(f"FAILURE: Node {port} missing foo{write_port}")
                  success = False

    if not success:
         for s in servers: s.kill()
         return 1

    print("Shutting down servers...")
    for s in servers:
        s.terminate()
        s.wait()

    print("Restarting servers...")
    servers = []
    # Restart same config
    servers.append(start_server(PORTS[0], PORTS[0], [PORTS[1], PORTS[2]]))
    servers.append(start_server(PORTS[1], PORTS[1], [PORTS[0], PORTS[2]]))
    servers.append(start_server(PORTS[2], PORTS[2], [PORTS[0], PORTS[1]]))

    print("Waiting for recovery (5s)...")
    time.sleep(5)

    print("Verifying persistence...")
    success = True
    for port in CLIENT_PORTS:
        # Verify all writes
        for write_port in CLIENT_PORTS:
             val = send_command(port, f"get foo{write_port}")
             if val != f"bar{write_port}":
                  print(f"FAILURE: Node {port} missing foo{write_port} (got {val})")
                  success = False

    print("Shutting down...")
    for s in servers:
        s.terminate()
        s.wait()

    if success:
        print("TEST PASSED")
        return 0
    else:
        print("TEST FAILED")
        return 1

if __name__ == "__main__":
    sys.exit(run_test())

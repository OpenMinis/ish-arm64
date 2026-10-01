#!/usr/bin/env python3
"""Minimal MCP stdio client, the way minis-mcp-cli talks to a server: start it, initialize,
tools/list, then a few tools/call. Prints one JSON line per server with the seconds to the
initialize answer (Minis gives a stdio server 60 s for it), to tools/list, per call ("!" marks a
call that answered with an error), and in total.

usage: mcp_probe.py <server>... | --all
The servers are the npm packages of images.json, installed globally. The recording uses the first
group; the second group is held out of it, to measure with.
"""
import json
import os
import select
import subprocess
import sys
import time

N = '/tmp/aojit/node'
SERVERS = {
    # name: (argv, env, [(tool, arguments)])
    'filesystem': (['mcp-server-filesystem', N], {}, [
        ('list_directory', {'path': N}),
        ('read_text_file', {'path': f'{N}/json_task.js', 'head': 20}),
        ('search_files', {'path': N, 'pattern': '*.js'}),
        ('get_file_info', {'path': f'{N}/fs_task.js'})]),
    'memory': (['mcp-server-memory'], {'MEMORY_FILE_PATH': '/tmp/mcp_memory.json'}, [
        ('create_entities', {'entities': [{'name': 'ish', 'entityType': 'project', 'observations': ['arm64 emulator']}]}),
        ('create_relations', {'relations': [{'from': 'ish', 'to': 'ish', 'relationType': 'self'}]}),
        ('search_nodes', {'query': 'emulator'}), ('read_graph', {})]),
    'everything': (['mcp-server-everything'], {}, [
        ('echo', {'message': 'hi'}), ('get-sum', {'a': 2, 'b': 3}), ('get-tiny-image', {}), ('get-env', {})]),
    'context7': (['context7-mcp'], {}, [('resolve-library-id', {'libraryName': 'express', 'query': 'routing middleware'})]),
    'mcp-remote': (['mcp-remote', 'https://mcp.deepwiki.com/mcp', '--transport', 'http-only'], {}, []),
    'firecrawl': (['firecrawl-mcp'], {'FIRECRAWL_API_KEY': 'fc-dummy'}, []),
    'github': (['mcp-server-github'], {}, [('search_repositories', {'query': 'ish-app language:c', 'perPage': 3})]),
    'npx-filesystem': (['npx', '-y', '@modelcontextprotocol/server-filesystem', N], {}, [('list_directory', {'path': N})]),
    # held out of the recording
    'sequential-thinking': (['mcp-server-sequential-thinking'], {}, [
        ('sequentialthinking', {'thought': 'step one', 'thoughtNumber': 1, 'totalThoughts': 2, 'nextThoughtNeeded': True}),
        ('sequentialthinking', {'thought': 'step two', 'thoughtNumber': 2, 'totalThoughts': 2, 'nextThoughtNeeded': False})]),
    'desktop-commander': (['desktop-commander'], {}, [
        ('list_directory', {'path': N}), ('read_file', {'path': f'{N}/text_task.js'})]),
    'notion': (['notion-mcp-server'], {'OPENAPI_MCP_HEADERS': '{"Authorization":"Bearer dummy","Notion-Version":"2022-06-28"}'}, []),
    'playwright': (['playwright-mcp', '--headless'], {}, []),
    'npx-github': (['npx', '-y', '@modelcontextprotocol/server-github'], {}, []),
}


def probe(name):
    argv, env, calls = SERVERS[name]
    t0 = time.time()
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         env=dict(os.environ, **env), bufsize=0)
    buf = b''

    def rpc(i, method, params, timeout):
        nonlocal buf
        p.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params}) + '\n').encode())
        end = time.time() + timeout
        while time.time() < end:
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get('id') == i:
                    return msg
            if select.select([p.stdout], [], [], 0.5)[0]:
                chunk = os.read(p.stdout.fileno(), 65536)
                if not chunk:
                    return {'error': 'eof'}
                buf += chunk
        return {'error': 'timeout'}

    out = {'server': name}
    m = rpc(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                              'clientInfo': {'name': 'aojit-probe', 'version': '1'}}, 300)
    out['init_s'], out['init_ok'] = round(time.time() - t0, 2), 'result' in m
    if not out['init_ok']:
        out['err'] = str(m.get('error'))[:120]
        p.kill()
        return out
    p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    t = time.time()
    m = rpc(2, 'tools/list', {}, 120)
    out['list_s'] = round(time.time() - t, 2)
    out['tools'] = len(m['result'].get('tools', [])) if 'result' in m else -1
    done = []
    for k, (tool, args) in enumerate(calls):
        t = time.time()
        m = rpc(10 + k, 'tools/call', {'name': tool, 'arguments': args}, 120)
        ok = 'result' in m and not m['result'].get('isError')
        done.append(f"{tool}:{round(time.time() - t, 2)}{'' if ok else '!'}")
    out['calls'], out['total_s'] = ' '.join(done), round(time.time() - t0, 2)
    p.stdin.close()
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        p.kill()
    return out


if __name__ == '__main__':
    names = list(SERVERS) if '--all' in sys.argv else sys.argv[1:]
    if not names:
        sys.exit(__doc__)
    for n in names:
        print(json.dumps(probe(n)), flush=True)

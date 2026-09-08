"""Exercise the actual bridge/broker IPC and UI lifecycle without a keyboard hook."""
import base64
import json
from pathlib import Path
import queue
import struct
import subprocess
import sys
import threading
import time

exe = str(Path(sys.argv[1]).resolve())
origin = 'chrome-extension://' + 'a' * 32 + '/'

class Peer:
	def __init__(self):
		self.process = subprocess.Popen([exe, origin], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
		self.messages = queue.Queue()
		def read():
			try:
				while True:
					header = self.process.stdout.read(4)
					if not header: break
					length, = struct.unpack('<I', header)
					self.messages.put(json.loads(self.process.stdout.read(length)))
			finally: self.messages.put(None)
		self.reader = threading.Thread(target=read, daemon=True)
		self.reader.start()
		assert self.next()['hostVersion'] == json.loads(Path('extension/manifest.json').read_text())['version']
	def send(self, message, fragmented=False):
		message = {'version': 1, **message}
		body = json.dumps(message).encode()
		frame = struct.pack('<I', len(body)) + body
		if fragmented:
			for part in (frame[:2], frame[2:9], frame[9:]):
				self.process.stdin.write(part); self.process.stdin.flush(); time.sleep(.03)
		else: self.process.stdin.write(frame); self.process.stdin.flush()
	def next(self):
		value = self.messages.get(timeout=10)
		if value is None: raise AssertionError('Unexpected EOF: ' + self.process.stderr.read().decode(errors='replace'))
		return value
	def snapshot(self, title, focused=False, active=5):
		self.send({'type': 'snapshot', 'focused': focused, 'activeTabId': active, 'tabs': [
			{'id': 5, 'windowId': 1, 'title': title, 'lastAccessed': 100},
			{'id': 6, 'windowId': 1, 'title': title + ' second', 'lastAccessed': 200}
		]}, fragmented=True)
		assert self.next()['type'] == 'synced'
	def inspect(self):
		self.send({'type': 'test_inspect'})
		result = self.next(); assert result['type'] == 'test_state'; return result
	def close(self):
		if self.process.poll() is None:
			self.process.stdin.close()
			try: self.process.wait(timeout=5)
			except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(); raise
		self.reader.join(timeout=2)

broker = subprocess.Popen([exe, '--test-broker'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
peers = []
try:
	time.sleep(.5)
	assert broker.poll() is None, broker.stderr.read().decode(errors='replace') if broker.poll() is not None else ''
	p = Peer(); peers.append(p)
	q = Peer(); peers.append(q)
	p.snapshot('Personal'); q.snapshot('Work')
	state = p.inspect()
	assert len(state['tabs']) == 4 and not state['visible'] and state['hiddenOwner'] and not state['specialWindowStyle']
	work = next(tab['client'] for tab in state['tabs'] if tab['title'] == 'Work')
	personal = next(tab['client'] for tab in state['tabs'] if tab['title'] == 'Personal')
	assert work != personal
	p.snapshot('Personal', focused=True)
	first = p.inspect()['tabs']
	p.snapshot('Personal renamed', focused=True)
	second = p.inspect()['tabs']
	assert [t['recent'] for t in first] == [t['recent'] for t in second], 'Title refresh changed MRU'
	q.snapshot('Work', focused=True, active=6)
	state = p.inspect()
	assert max(state['tabs'], key=lambda tab: tab['recent'])['client'] == work
	preview = base64.b64encode(Path('tests/fixtures/preview.jpg').read_bytes()).decode()
	p.send({'type': 'preview', 'tabId': 5, 'image': preview})
	assert p.next()['type'] == 'previewed'
	state = p.inspect()
	assert next(t['preview'] for t in state['tabs'] if t['client'] == personal and t['id'] == 5)
	assert not next(t['preview'] for t in state['tabs'] if t['client'] == work and t['id'] == 5)
	assert all(t['previewsEnabled'] is None for t in state['tabs']), 'Unknown permission must not imply disabled'
	p.send({'type': 'preview_state', 'enabled': False})
	state = p.inspect()
	assert all(t['previewsEnabled'] is False and not t['preview'] for t in state['tabs'] if t['client'] == personal)
	assert all(t['previewsEnabled'] is None for t in state['tabs'] if t['client'] == work), 'Permission leaked across profiles'
	p.send({'type': 'preview_state', 'enabled': True})
	assert all(t['previewsEnabled'] is True for t in p.inspect()['tabs'] if t['client'] == personal)
	p.send({'type': 'preview', 'tabId': 5, 'image': preview})
	assert p.next()['type'] == 'previewed'
	p.send({'type': 'test_cycle', 'direction': 1})
	opened = p.inspect()
	assert opened['visible'] and opened['preparedFrames'] > 0 and opened['preparedWhileHidden']
	if opened['effectsEnabled']:
		assert opened['composedFrame'], 'Cards must be presented above the compositor background'
	else:
		assert opened['backgroundPixel'] in [0xff303030, 0xffebebeb], 'Disabled effects must use an opaque themed background, not black'
	assert opened['dwmFrame'], 'Acrylic requires DWM frame rendering for the production owned popup'
	assert opened['borderless'] and opened['hiddenOwner'] and not opened['specialWindowStyle'], 'DWM integration must not add window chrome or a helper entry'
	for direction in [1, -1, 1, -1]:
		p.send({'type': 'test_cycle', 'direction': direction})
		cycled = p.inspect()
		assert cycled['thumbnailBuilds'] == opened['thumbnailBuilds'], 'Selection rebuilt live thumbnails'
		assert cycled['preparedFrames'] == opened['preparedFrames'], 'Selection rebuilt the opening frame'
		assert cycled['frameAllocations'] == opened['frameAllocations'], 'Selection allocated a new frame buffer'
		assert cycled['surfaceAllocations'] == opened['surfaceAllocations'], 'Selection replaced the GPU surface'
	p.send({'type': 'preview', 'tabId': 5, 'image': preview})
	assert p.next()['type'] == 'previewed'
	assert p.inspect()['thumbnailBuilds'] == opened['thumbnailBuilds'], 'JPEG refresh rebuilt window thumbnails'
	p.send({'type': 'test_cancel'})
	assert not p.inspect()['visible']
	# Every opening must survive its initial foreground handoff.
	for _ in range(5):
		p.send({'type': 'test_cycle', 'direction': 1})
		assert p.inspect()['visible'], 'First cycle cancelled during focus handoff'
		p.send({'type': 'test_cancel'})
		assert not p.inspect()['visible']
	p.send({'type': 'test_select', 'client': work, 'tabId': 5})
	assert q.next() == {'type': 'activate', 'version': 1, 'tabId': 5}
	q.send({'type': 'result', 'ok': True})
	assert not p.inspect()['visible'], 'Overlay remained visible after selection'
	p.send({'type': 'test_close', 'client': work, 'tabId': 6})
	assert q.next() == {'type': 'close', 'version': 1, 'tabId': 6}
	q.send({'type': 'result', 'ok': True})
	assert not p.inspect()['visible'], 'Overlay remained visible after close request'
	p.send({'type': 'clear_previews'})
	assert not any(t['preview'] for t in p.inspect()['tabs'])
	p.close(); peers.remove(p)
	time.sleep(.2)
	assert len(q.inspect()['tabs']) == 2, 'Disconnected profile tabs leaked'
	# Malformed frames must disconnect only the offending profile.
	def frame(value):
		body = json.dumps(value).encode()
		return struct.pack('<I', len(body)) + body
	valid = {'type': 'snapshot', 'version': 1, 'activeTabId': 1, 'tabs': [{'id': 1, 'windowId': 1, 'title': 'Example'}]}
	invalid = [struct.pack('<I', 0), struct.pack('<I', 1024 * 1024 + 1),
		struct.pack('<I', 2) + b'\xff\xff', frame([]), frame({}),
		frame({**valid, 'version': 2}), frame({**valid, 'activeTabId': 99}),
		frame({**valid, 'tabs': valid['tabs'] * 2}), frame({**valid, 'activeTabId': 1.5}),
		frame({**valid, 'tabs': [{'id': 1, 'windowId': 1, 'title': 'x' * 1025}]})]
	for data in invalid:
		bad = Peer(); peers.append(bad)
		bad.process.stdin.write(data); bad.process.stdin.flush()
		assert bad.messages.get(timeout=5) is None, 'Malformed input was accepted'
		bad.close(); peers.remove(bad)
		assert len(q.inspect()['tabs']) == 2, 'Bad peer broke healthy profile'
	for encoded in ['not base64!', base64.b64encode(b'not a JPEG').decode(), 'A' * 174765]:
		bad = Peer(); peers.append(bad); bad.snapshot('Invalid preview')
		bad.send({'type': 'preview', 'tabId': 5, 'image': encoded})
		assert bad.messages.get(timeout=5) is None, 'Invalid preview was accepted'
		bad.close(); peers.remove(bad)
	print('PASS: fragmented IPC, two profiles, MRU stability, previews, exact-profile activation, hidden owner, disconnect and error isolation')
finally:
	for peer in peers:
		try: peer.close()
		except Exception: pass
	if broker.poll() is None: broker.terminate()
	broker.wait(timeout=5)

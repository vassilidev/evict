// Checks the page's path rules (what can be deleted, categories, how paths read) on Windows, macOS and Linux paths
const fs = require('fs');
const src = fs.readFileSync('index.html', 'utf8').split('<script>')[2];
const head = src.slice(0, src.indexOf('// Minimal Markdown'));
function load(os) {
  const window = {DATA: {os, tree: ['/', 0, []], home: '/C:/Users/me'}}, document = {getElementById: () => ({}), body: {}};
  return new Function('window', 'document', head + '; return {canRm, SYS, JUNK, show, DISK, isDir, ARCH}')(window, document);
}
const w = load('win'), m = load('mac'), assert = require('assert');
const yes = ['/C:/Users/me/Downloads/big.iso', '/C:/Users/me/AppData/Local/npm-cache', '/C:/$Recycle.Bin/S-1-5-21-1/$Rabc.txt', '/D:/Games/Old', '/C:/Users/me/Documents/x', '/C:/Program Files/Foo', '/C:/ProgramData/Docker'];
const no = ['/C:', '/C:/Users', '/C:/Users/me', '/C:/Games', '/C:/Windows/Temp/x', '/C:/Users/me/AppData', '/C:/Users/me/AppData/Local', '/C:/Users/me/.ssh/id_rsa',
  '/C:/Users/me/AppData/Roaming/Microsoft/Protect/S-1', '/C:/$Recycle.Bin/S-1-5-21-1', '/C:/$WINDOWS.~BT/Sources', '/C:/System Volume Information/x', '/C:/Users/me/AppData/Local/Temp', '/'];
for (const p of yes) assert.ok(w.canRm(p), 'should delete ' + p);
for (const p of no) assert.ok(!w.canRm(p), 'should protect ' + p);
assert.ok(m.canRm('/Users/me/Downloads/x') && !m.canRm('/Users/me') && !m.canRm('/Applications') && m.canRm('/Applications/Foo.app'));
assert.ok(w.SYS.test('/C:/Windows/System32') && !w.SYS.test('/C:/$Recycle.Bin/x') && w.SYS.test('/C:/$WinREAgent') && !w.SYS.test('/C:/Users'));
assert.equal(w.show('/C:/Users/me'), 'C:\\Users\\me'); assert.equal(w.show('/C:'), 'C:\\'); assert.equal(w.show('/'), 'This PC');
assert.equal(m.show('/Users/me'), '/Users/me');
assert.ok(w.JUNK.test('$Recycle.Bin') && w.ARCH.test('setup.msi') && w.ARCH.test('disk.vhdx'));
console.log('rules OK');

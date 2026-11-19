"""Generate fixed public test vectors with the upstream Espressif SRP client.

All passwords/ephemerals here are public fixtures, never manufacturing defaults.
AES-GCM oracle is Python cryptography/OpenSSL, separate from browser Noble.
"""
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parent / 'reference'))
import srp6a
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

vectors = []
for index in [0, 1, 2, 3, 4, 6, 7, 8]:
    username = 'setup-fixture-' + str(index)
    password = 'Public-Test-Only-' + str(index)
    salt = hashlib.sha256(('salt-' + str(index)).encode()).digest()[:16]
    secret = bytearray(hashlib.sha256(('client-' + str(index)).encode()).digest())
    secret[0] |= 128
    secret_int = int.from_bytes(secret, 'big')
    srp6a.get_random_of_length = lambda n, fixed=secret_int: fixed
    client = srp6a.Srp6a(username, password)
    n,g = srp6a.get_ng(srp6a.NG_3072)
    server_secret = int.from_bytes(hashlib.sha256(('server-' + str(index)).encode()).digest(),'big')
    x = srp6a.calculate_x(hashlib.sha512,salt,username,password)
    verifier = pow(g,x,n)
    server_public = (client.k * verifier + pow(g,server_secret,n)) % n
    public = srp6a.long_to_bytes(server_public)
    proof = client.process_challenge(salt,public)
    client.verify_session(client.H_AMK)
    key = client.get_session_key()[:32]
    nonce = hashlib.sha256(('nonce-' + str(index)).encode()).digest()[:8] + b'\0\0\0\1'
    plaintext = ('{"operation":"GET_STATE","fixture":'+str(index)+'}').encode()
    ciphertext = AESGCM(key).encrypt(nonce,plaintext,None)
    server_nonce=nonce[:8]+(2).to_bytes(4,'big')
    reply=b'{"code":"OK"}'
    encrypted_reply=AESGCM(key).encrypt(server_nonce,reply,None)
    vectors.append({
        'username':username,'password':password,'salt':salt.hex(),
        'ephemeral':bytes(secret).hex(),'clientPublic':srp6a.long_to_bytes(client.A).hex(),
        'serverPublic':public.hex(),'clientProof':proof.hex(),'serverProof':client.H_AMK.hex(),
        'key':key.hex(),'nonce':nonce.hex(),'plaintext':plaintext.hex(),'ciphertext':ciphertext.hex(),
        'reply':reply.hex(),'encryptedReply':encrypted_reply.hex(),
    })
path=Path(__file__).parent/'security2-vectors.json'
path.write_text(json.dumps(vectors,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Generated',len(vectors),'independent Espressif/Python AES-GCM vectors')

# Regression against the device C formulas, independent of the Python client's
# calculate_x integer conversion bug. Keep leading-zero hash and salt bytes.
edge=[]
for index,salt in [(5,hashlib.sha256(b'salt-5').digest()[:16]),(9,b'\0'+hashlib.sha256(b'leading-zero-salt').digest()[:15])]:
    username='setup-fixture-'+str(index);password='Public-Test-Only-'+str(index)
    raw=bytearray(hashlib.sha256(('client-'+str(index)).encode()).digest());raw[0]|=128
    a=int.from_bytes(raw,'big');A=pow(g,a,n);pad=lambda i:i.to_bytes(384,'big')
    h=lambda *parts:hashlib.sha512(b''.join(parts)).digest()
    x=int.from_bytes(h(salt,h((username+':'+password).encode())),'big')
    v=pow(g,x,n);b=int.from_bytes(hashlib.sha256(('server-'+str(index)).encode()).digest(),'big')
    k=int.from_bytes(h(pad(n),pad(g)),'big');B=(k*v+pow(g,b,n))%n;Bbytes=srp6a.long_to_bytes(B)
    u=int.from_bytes(h(pad(A),pad(B)),'big')
    # Compute S using SERVER algebra, not the browser client expression.
    S=pow(A*pow(v,u,n),b,n);K=h(srp6a.long_to_bytes(S))
    M=h(bytes(a^b for a,b in zip(h(pad(n)),h(pad(g)))),h(username.encode()),salt,pad(A),Bbytes,K)
    proof=h(pad(A),M,K)
    edge.append({'username':username,'password':password,'ephemeral':bytes(raw).hex(),'salt':salt.hex(),'serverPublic':Bbytes.hex(),'clientProof':M.hex(),'serverProof':proof.hex(),'key':K[:32].hex()})
(path.parent/'security2-device-edge-vectors.json').write_text(json.dumps(edge,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Generated',len(edge),'device C algebra leading-zero regression vectors')

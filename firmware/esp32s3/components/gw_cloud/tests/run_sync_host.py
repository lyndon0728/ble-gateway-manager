"""Exercise production inventory, recovery and manufacturing trust on the host.

Device I/O is mocked; cJSON, scan compilation, SHA256, canonicalization and
Ed25519 are the production sources. Python supplies independent signed fixtures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def fixtures():
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    import base64
    private = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    device = dict(gatewayId="gw_test", sn="SN-1", model="GW32I33V2", hardwareVersion="2", capabilities=["CONFIGURE", "CONFIG_SNAPSHOT_1", "NRF54_BLE_FILTER_V2"], credentialHash="a"*64)
    manifest = canonical(dict(schemaVersion="1.0", devices=[device]))
    artifact = canonical(dict(keyId="manufacturing-test", kind="DEVICE_MANIFEST", name="factory", version="1", sha256=hashlib.sha256(manifest.encode()).hexdigest()))
    def signed_pair(m):
        raw=canonical(m)
        outer=canonical(dict(keyId="manufacturing-test",kind="DEVICE_MANIFEST",name="factory",version="1",sha256=hashlib.sha256(raw.encode()).hexdigest()))
        return dict(manifest=raw,artifact=outer,signature=base64.b64encode(private.sign(outer.encode())).decode())
    invalid_device = dict(device, capabilities=["lowercase"])
    scan=dict(scan=dict(enabled=True,active=True,intervalMs=100,windowMs=50),filters=dict(minRssi=None,serviceUuids=[],manufacturerIds=[]),beaconRules=[],beaconFilterV2=None)
    advertising=dict(enabled=False,mode="NONCONNECTABLE",intervalMs=100,advDataBase64="",scanResponseBase64="")
    network=dict(uplinkOrder=["ETHERNET"],ethernet=dict(enabled=True,ipv4=dict(mode="DHCP")),wifiSta=dict(enabled=False,ipv4=dict(mode="DHCP"),ssid="网关\u2028<&\\u2028",security="WPA2_PSK",credentialRef="cred-1"))
    values=dict(network=network,nrf54Scan=scan,nrf54Advertising=advertising,esp32Scan=scan,esp32Advertising=advertising,upload=dict(batchMaxRecords=32,flushIntervalMs=1000))
    groups={k:dict(revision=1000000000000,value=v,source=dict(kind="VERIFIED_TASK",id="task-"+k),verifiedAt="2026-11-19T00:00:00.000Z") for k,v in values.items()}
    snapshot=dict(schemaVersion="1.0",namespaceId="ns_test",gatewayId="gw_test",snapshotId="snapshot-1",generation="9007199254740993",complete=True,groups=groups,missingGroups=[],blockedReasons=[])
    snapshot["digest"]=hashlib.sha256(canonical(snapshot).encode()).hexdigest()
    max_integer=dict(revision=9007199254740991,utf8="网关\u2028<&\\u2028",negative=0)
    firmware=canonical(dict(keyId="firmware-test",kind="FIRMWARE",name="factory-fw",version="1.0",component="GATEWAY",sha256="b"*64,sizeBytes=128,compatibleModels=["GW32I33V2"],hardwareVersions=["2"]))
    return dict(manifest=manifest,artifact=artifact,signature=base64.b64encode(private.sign(artifact.encode())).decode(),publicKey=base64.b64encode(public).decode(),duplicate=signed_pair(dict(schemaVersion="1.0",devices=[device,device])),invalidCapability=signed_pair(dict(schemaVersion="1.0",devices=[invalid_device])),snapshot=snapshot,maxInteger=max_integer,maxIntegerDigest=hashlib.sha256(canonical(max_integer).encode()).hexdigest(),factoryFirmware=dict(manifest=firmware,signature=base64.b64encode(private.sign(firmware.encode())).decode(),artifactId="artifact-local"))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc",default="cc")
    parser.add_argument("--idf",required=True,type=Path)
    args=parser.parse_args()
    tests=Path(__file__).resolve().parent
    components=tests.parent.parent
    repo=components.parents[2]
    cjson=args.idf.resolve()/"components/json/cJSON"
    mbed=args.idf.resolve()/"components/mbedtls/mbedtls"
    vendor=components/"gw_actions/vendor/monocypher"
    includes=[tests/"sync_mocks",tests/"mocks"]+[components/name/"include" for name in ("gw_core","gw_nrf","gw_wire","gw_http","gw_authority")]+[cjson,mbed/"include",components/"gw_capture/tests/mocks",components/"gw_actions/tests/action_stubs",vendor]
    cc=[args.cc]+(["cc"] if Path(args.cc).stem=="zig" else [])
    flags=["-std=c11","-D_GNU_SOURCE","-Wall","-Wextra","-Werror",'-DMBEDTLS_CONFIG_FILE="test_mbedtls_config.h"']+["-I"+str(i) for i in includes]
    crypto=[mbed/"library"/(n+".c") for n in ("sha256","base64","platform_util","platform","constant_time")]
    with tempfile.TemporaryDirectory(prefix="gw-sync-host-") as directory:
        out=Path(directory);fixture=out/"fixtures.json";fixture.write_text(json.dumps(fixtures(),ensure_ascii=False),encoding="utf-8")
        for name in ("sync","authority"):
            source=tests/"test_sync.c" if name=="sync" else components/"gw_authority/tests/test_authority.c"
            sources=[source,cjson/"cJSON.c",*crypto]
            if name=="sync":sources += [components/"gw_core/gw_validate.c",components/"gw_core/gw_rules.c",components/"gw_nrf/gw_scan_config.c",repo/"firmware/nrf54l15/src/gw_core.c",repo/"firmware/nrf54l15/src/gw_filter_v2.c"]
            else:sources += [vendor/"monocypher.c",vendor/"monocypher-ed25519.c"]
            binary=out/(name+(".exe" if os.name=="nt" else ""))
            subprocess.run([*cc,*flags,*map(str,sources),"-lm","-o",str(binary)],check=True)
            subprocess.run([str(binary),str(fixture)],check=True,timeout=60)
    print("PASS: production cloud synchronization and manufacturing authority contracts")


if __name__=="__main__":main()

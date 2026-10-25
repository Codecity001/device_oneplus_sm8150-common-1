#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import tempfile
from functools import partial
from os import path
from pathlib import Path
from zipfile import ZipFile

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)
from extract_utils.tools import (
    apktool_path,
    build_tools_dir,
    java_path,
)
from extract_utils.utils import run_cmd

namespace_imports = [
    'device/oneplus/sm8150-common',
    'hardware/oplus',
    'hardware/qcom-caf/sm8150',
    'hardware/qcom-caf/wlan',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/qcom/opensource/commonsys/display',
    'vendor/qcom/opensource/dataservices',
    'vendor/qcom/opensource/display',
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'libmmosal',
        'vendor.qti.hardware.wifidisplaysession@1.0',
        'vendor.qti.imsrtpservice@3.0',
    ): lib_fixup_vendor_suffix,
}


def replace_patched_dex(
    patch: str, ctx, file, file_path: str, *args, tmp_dir: str, **kwargs
):
    # Swap only the reassembled dex and manifest into the original APK:
    # apktool_pack() would drop its v1 signature and the page alignment
    # of its stored native libraries.
    with open(path.join(ctx.module_dir, patch)) as f:
        patched_targets = {
            line[len('+++ b/') :].strip()
            for line in f
            if line.startswith('+++ b/')
        }
    smali_dirs = {
        target.split('/', 1)[0]
        for target in patched_targets
        if target.startswith('smali')
    }
    dex_files = [
        'classes.dex' if d == 'smali' else d.removeprefix('smali_') + '.dex'
        for d in sorted(smali_dirs)
    ]
    has_manifest = any(target == 'AndroidManifest.xml' for target in patched_targets)

    run_cmd([java_path, '-jar', apktool_path, 'b', '--no-apk', tmp_dir])
    built_dir = path.join(tmp_dir, 'build', 'apk')
    zip_args = ['-C', built_dir]
    for dex in dex_files:
        zip_args += ['-f', path.join(built_dir, dex)]
    if has_manifest:
        zip_args += ['-f', path.join(built_dir, 'AndroidManifest.xml')]

    entries = [info.filename for info in ZipFile(file_path).infolist()]
    # zip2zip takes each name as a glob or an in:out mapping.
    assert not any(set(name) & set('*?[]{}\\:') for name in entries)

    soong_zip, merge_zips, zip2zip, zipalign = (
        path.join(build_tools_dir, tool)
        for tool in ('soong_zip', 'merge_zips', 'zip2zip', 'zipalign')
    )
    with tempfile.TemporaryDirectory() as work_dir:
        patch_zip = path.join(work_dir, 'patch.zip')
        merged = path.join(work_dir, 'merged.zip')
        ordered = path.join(work_dir, 'ordered.zip')

        run_cmd([soong_zip, '-o', patch_zip, '-L', '0'] + zip_args)
        run_cmd([merge_zips, '-ignore-duplicates', merged, patch_zip, file_path])
        run_cmd([zip2zip, '-t', '-i', merged, '-o', ordered] + entries)
        run_cmd([zipalign, '-f', '-p', '4', ordered, file_path])


def apktool_patch_dex(patch: str) -> blob_fixup:
    return (
        blob_fixup()
        .apktool_unpack(patch)
        .patch_dir(patch)
        .call(partial(replace_patched_dex, patch))
    )


blob_fixups: blob_fixups_user_type = {
    'odm/bin/hw/vendor.oplus.hardware.biometrics.fingerprint@2.1-service': blob_fixup()
        .add_needed('libshims_fingerprint.oplus.so'),
    'odm/etc/vintf/manifest/manifest_oplus_fingerprint.xml': blob_fixup()
        .patch_file('blob-patches/manifest_oplus_fingerprint.patch'),
    ('odm/lib/liba2dpoffload.so', 'odm/lib/libaudioEngineerTest.so', 'vendor/lib/hw/sound_trigger.primary.msmnile.so', 'vendor/lib/libhdmipassthru.so', 'vendor/lib/libssrec.so'): blob_fixup()
        .replace_needed('libaudioroute.so', 'libaudioroute-v34.so'),
    ('odm/lib64/mediadrm/libwvdrmengine.so', 'odm/lib64/libwvhidl.so'): blob_fixup()
        .add_needed('libcrypto_shim.so'),
    'vendor/lib64/libarcsoft_super_night_raw.so': blob_fixup()
        .clear_symbol_version('remote_handle_close')
        .clear_symbol_version('remote_handle_invoke')
        .clear_symbol_version('remote_handle_open')
        .clear_symbol_version('remote_register_buf_attr')
        .clear_symbol_version('remote_register_buf'),
    'product/app/PowerOffAlarm/PowerOffAlarm.apk': blob_fixup()
        .apktool_patch('blob-patches/PowerOffAlarm.patch'),
    'product/etc/sysconfig/com.android.hotwordenrollment.common.util.xml': blob_fixup()
        .regex_replace('/my_product', '/product'),
    'product/priv-app/OnePlusCamera/OnePlusCamera.apk': apktool_patch_dex('blob-patches/OnePlusCamera.patch'),
    'system/framework/WfdCommon.jar': blob_fixup()
        .apktool_patch('blob-patches/WfdCommon.patch'),
    'system_ext/framework/oplus-ims-ext.jar': blob_fixup()
        .apktool_patch('blob-patches/oplus-ims-ext.patch'),
    'system_ext/lib64/libwfdservice.so': blob_fixup()
        .replace_needed('android.media.audio.common.types-V4-cpp.so', 'android.media.audio.common.types-V5-cpp.so'),
    'vendor/app/IWlanService/IWlanService.apk': blob_fixup()
        .apktool_patch('blob-patches/IWlanService-null-credentials.patch'),
    'vendor/bin/hw/vendor.dolby.hardware.dms@2.0-service': blob_fixup()
        .add_needed('libstagefright_foundation-v33.so'),
    'vendor/etc/init/vendor.qti.adsprpc-service.rc': blob_fixup()
        .patch_file('blob-patches/vendor.qti.adsprpc-service.rc.patch'),
    'vendor/etc/init/vendor.qti.cdsprpc-service.rc': blob_fixup()
        .patch_file('blob-patches/vendor.qti.cdsprpc-service.rc.patch'),
    'vendor/etc/init/vendor.sensors.sscrpcd.rc': blob_fixup()
        .patch_file('blob-patches/vendor.sensors.sscrpcd.rc.patch'),
    'vendor/etc/init/vppservice.rc': blob_fixup()
        .patch_file('blob-patches/vppservice.rc.patch'),
    'vendor/etc/sensors/sns_reg_config': blob_fixup()
        .regex_replace('version=8', 'version=9'),
    'vendor/etc/libnfc-nci.conf': blob_fixup()
        .regex_replace('NFC_DEBUG_ENABLED=0x01', 'NFC_DEBUG_ENABLED=0x00'),
    'vendor/etc/libnfc-nxp.conf': blob_fixup()
        .regex_replace('NXP_NFC_DEV_NODE="/dev/pn553"', 'NXP_NFC_DEV_NODE="/dev/nq-nci"')
        .regex_replace('(NXPLOG_.*_LOGLEVEL)=0x03', '\\1=0x02')
        .regex_replace('NFC_DEBUG_ENABLED=0x01', 'NFC_DEBUG_ENABLED=0x00'),
    'vendor/etc/seccomp_policy/wfdhdcphalservice.policy': blob_fixup()
        .add_line_if_missing('gettid: 1'),
    'vendor/etc/wfdconfig.xml': blob_fixup()
        .regex_replace('<AudioStreamInSuspend>0</AudioStreamInSuspend>', '<AudioStreamInSuspend>1</AudioStreamInSuspend>')
        .regex_replace('<HID>0</HID>', '<HID>1</HID>'),
    'vendor/lib/hw/sound_trigger.primary.msmnile.so': blob_fixup()
        .binary_regex_replace(b'/vendor/lib/hw\x00', b'/odm/lib/hw\x00\x00\x00\x00'),
    'vendor/lib64/libdpps.so': blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2_1.so')
}  # fmt: skip

module = ExtractUtilsModule(
    'sm8150-common',
    'oneplus',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)


def add_custom_modules(ctx, packages_ctx):
    ctx.bp_out.write(Path(__file__).with_name('proprietary-files.bp.in').read_text())
    ctx.product_mk_out.write(
        '\nPRODUCT_PACKAGES += \\\n'
        '    libaudiohal@6.0 \\\n'
        '    prebuilt_libCameraMDMHelper@2.0\n'
    )


module.proprietary_files[0].add_post_makefile_generation_fn(add_custom_modules)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()

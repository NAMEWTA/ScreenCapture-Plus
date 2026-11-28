"""The exact case-distinct files in the original musl target header archive.

This admits no general case-sensitive mode. Ordinary sources, deployed packages
and Windows paths keep their existing case-collision rejection.
"""
from __future__ import annotations

import sys

COMPONENT = 'appimage-linux-headers'
ARCHIVE_PREFIX = 'linux-headers-4.19.88-2'
ARCHIVE_SHA256 = 'dc7abf734487553644258a3822cfd429d74656749e309f2b25f09f4282e05588'
TREE_SHA256 = 'c0f1c4e0a7d96e99bc8dd3202229a74f47137b2e147afd68205065c520ab1004'
PROFILE = 'linux-headers-4.19.88-2-case-v1'
PROFILE_RECORD = {'profile': PROFILE, 'sourceTreeSha256': TREE_SHA256}
SOURCE = PROFILE + '-source'
SYSROOT = PROFILE + '-sysroot'
BUNDLE = PROFILE + '-bundle'

# These sixteen regular-file identities were read from the exact original
# archive above. Upper/lowercase forms are distinct Linux UAPI headers.
CASE_FILES = {
    "generic/include/linux/netfilter/xt_CONNMARK.h": {
        "mode": 420,
        "sha256": "e15d96fe80c7dbe980e0620920161e667af83993399e7cbb74f30957088ea204",
        "size": 199
    },
    "generic/include/linux/netfilter/xt_DSCP.h": {
        "mode": 420,
        "sha256": "d6fab82c38205addd40f4515d4c1081b51eefbe549c69047da7f38b1dfc6ceb5",
        "size": 697
    },
    "generic/include/linux/netfilter/xt_MARK.h": {
        "mode": 420,
        "sha256": "e0662377a6c673e8f2e6f3c7539417924902950a2e4a753a73940cc871789bdc",
        "size": 184
    },
    "generic/include/linux/netfilter/xt_RATEEST.h": {
        "mode": 420,
        "sha256": "35e2457f0cd20c1ed924da4c8c1b6ecbde34c15175426ff7a7cc24f251689ad2",
        "size": 390
    },
    "generic/include/linux/netfilter/xt_TCPMSS.h": {
        "mode": 420,
        "sha256": "db3bcf49d2a9f71386faee27be40b021cc0d3703c5d435c3e46dce5ec60595c4",
        "size": 235
    },
    "generic/include/linux/netfilter/xt_connmark.h": {
        "mode": 420,
        "sha256": "6187776230ec618204bd40b9d244402f06023af1ea58c7136576c5e1405c73e0",
        "size": 900
    },
    "generic/include/linux/netfilter/xt_dscp.h": {
        "mode": 420,
        "sha256": "73ece7f43c6adfe0a55351d2d795ca656be6aa98f3b09d99c8819fff2b5ef591",
        "size": 701
    },
    "generic/include/linux/netfilter/xt_mark.h": {
        "mode": 420,
        "sha256": "eaac1d30d0f595a9cc160a8555d7b8ec7c9d3dc376eb8e6d29ecc3271ac029a2",
        "size": 260
    },
    "generic/include/linux/netfilter/xt_rateest.h": {
        "mode": 420,
        "sha256": "278f7a6d6f22e392ef8965b017e1523903d14236c18c5a84136c5389cafcfd58",
        "size": 859
    },
    "generic/include/linux/netfilter/xt_tcpmss.h": {
        "mode": 420,
        "sha256": "89038386cb5271e68a462237f4bff84fff467327795588de35a0d4d1d5aa7df2",
        "size": 253
    },
    "generic/include/linux/netfilter_ipv4/ipt_ECN.h": {
        "mode": 420,
        "sha256": "0be3d696de201a826d7c7042754bb5a702cf9c38f960a04902b95b1be0750139",
        "size": 901
    },
    "generic/include/linux/netfilter_ipv4/ipt_TTL.h": {
        "mode": 420,
        "sha256": "e73ff75cc058bca61495d0eb34253c5cdd67908b94b06bedae485c1129d6917a",
        "size": 375
    },
    "generic/include/linux/netfilter_ipv4/ipt_ecn.h": {
        "mode": 420,
        "sha256": "45bc9787ce575fc4b7ed4b980f3c6e304f255d706ac18ad4fcd77b32a575e323",
        "size": 431
    },
    "generic/include/linux/netfilter_ipv4/ipt_ttl.h": {
        "mode": 420,
        "sha256": "4a65de567a2b659c32538b0b07da545caf2b30e2cf026c20680c006bd951df72",
        "size": 431
    },
    "generic/include/linux/netfilter_ipv6/ip6t_HL.h": {
        "mode": 420,
        "sha256": "6a87f3bd765a1d5e73de80c92897872adde8baad2953d6e9339fe6aa30e72f07",
        "size": 408
    },
    "generic/include/linux/netfilter_ipv6/ip6t_hl.h": {
        "mode": 420,
        "sha256": "f6d1d4e3c03dff041eb00baa7fe48bdb5da14bd1b0e7201a7dde41eee4a44290",
        "size": 458
    }
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def entries(profile):
    if profile is None:
        return {}
    require(sys.platform.startswith('linux') and profile in (SOURCE, SYSROOT, BUNDLE),
            'Only the reviewed original Linux header path profiles are supported')
    def name(path):
        if profile == SYSROOT:
            return 'x86_64-linux-musl/include/' + path.removeprefix('generic/include/')
        return ('sources/' + COMPONENT + '/' if profile == BUNDLE else '') + path
    return {name(path): {'type':'file', **identity} for path, identity in CASE_FILES.items()}


def source_profile(component, pin, platform=None):
    if component != COMPONENT and 'sourcePathProfile' not in pin:
        return None
    require(component == COMPONENT and platform in (None, 'linux') and
            pin.get('archivePrefix') == ARCHIVE_PREFIX and
            pin.get('sourceArchive', {}).get('sha256') == ARCHIVE_SHA256 and
            pin.get('sourcePathProfile') == PROFILE_RECORD and not pin.get('patches'),
            'The header path exception requires its exact original Linux archive and complete tree')
    return SOURCE


def source_inventory(component, pin, directory, api):
    profile = source_profile(component, pin)
    files = api['inventory'](directory, path_profile=profile)
    if profile:
        require(api['tree_digest'](files) == TREE_SHA256,
                'The complete original Linux header source tree changed')
    return files

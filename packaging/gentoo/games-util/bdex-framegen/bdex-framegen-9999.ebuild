# Copyright 2026 Bdexez
# Proprietary software, all rights reserved (see LICENSE)

EAPI=8

# The layer for each ABI (abi_x86_32: the 32-bit one, for 32-bit games),
# the launcher, panel and profiles once. A local overlay:
#   packaging/gentoo is one (metadata/layout.conf); tools/ci/gentoo-check.sh

inherit cmake-multilib

DESCRIPTION="Frame generation and upscaling Vulkan layer for games"
HOMEPAGE="https://github.com/Bdexez/bdex_frame_gen"
if [[ ${PV} == 9999 ]]; then
	inherit git-r3
	EGIT_REPO_URI="https://github.com/Bdexez/bdex_frame_gen.git"
else
	SRC_URI="https://github.com/Bdexez/bdex_frame_gen/archive/v${PV}.tar.gz -> ${P}.tar.gz"
	S="${WORKDIR}/bdex_frame_gen-${PV}"
	KEYWORDS="~amd64 ~arm64"
fi

LICENSE="all-rights-reserved"
RESTRICT="bindist mirror"
SLOT="0"
IUSE="gui"

DEPEND="dev-util/vulkan-headers"
RDEPEND="
	media-libs/vulkan-loader[${MULTILIB_USEDEP}]
	gui? (
		dev-lang/python:*
		dev-python/pygobject:3
		gui-libs/gtk:4[introspection]
	)
"
BDEPEND="|| ( media-libs/shaderc dev-util/glslang )"

multilib_src_configure() {
	local mycmakeargs=(
		-DBDEX_BUILD_DEMO=OFF
		-DBDEX_BUILD_TESTS=OFF
		-DBDEX_BUILD_OFFLINE=OFF
		-DBDEX_INSTALL_LIBDIR=$(get_libdir)
	)
	# The other ABI's layer: its own manifest (VK_LAYER_BDEX_framegen_32,
	# library_arch 32); the eclass already passes -m32.
	multilib_is_native_abi || mycmakeargs+=( -DBDEX_32BIT=ON )
	cmake_src_configure
}

multilib_src_install_all() {
	einstalldocs
	if ! use gui; then
		rm -f "${ED}"/usr/bin/bdex-framegen-gui "${ED}"/usr/share/applications/bdex-framegen.desktop || die
	fi
}

pkg_postinst() {
	elog "The layer loads into every Vulkan game and stays inert until turned on:"
	elog "  bdex-framegen-gui (USE=gui), or BDEX_FG=1 %command% in Steam's launch options."
	elog "Steam, Heroic, Lutris or Bottles installed as a Flatpak? Run once, as your user:"
	elog "  bdex-framegen --sandboxes"
}

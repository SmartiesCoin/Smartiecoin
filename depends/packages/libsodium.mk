package=libsodium
# Pin the stable-branch snapshot to an immutable commit, not the daily archive.
# This commit archive matches the signed upstream 1.0.22-stable snapshot.
$(package)_version=1.0.22-stable-9dcaa81943641565585482c20f2a211200aa1d89
$(package)_download_path=https://github.com/jedisct1/libsodium/archive
$(package)_download_file=9dcaa81943641565585482c20f2a211200aa1d89.tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=52a93c23309300e55c3871d1592aa3a890321eafcfe6e84669631a154e626702

define $(package)_set_vars
  $(package)_config_opts=--disable-shared --enable-static
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

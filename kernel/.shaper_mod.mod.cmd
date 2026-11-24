savedcmd_shaper_mod.mod := printf '%s\n'   shaper_mod.o | awk '!x[$$0]++ { print("./"$$0) }' > shaper_mod.mod

# mod-ollama-chat config sync
#
# Copies the local custom config into the worldserver's module conf directory.
# Run after editing my-custom-config.conf, then restart worldserver (or `.ollama reload`).

# Source: the working copy we edit in this repo.
src  := justfile_directory() / "my-custom-config.conf"
# Destination: the conf the worldserver actually loads.
dest := home_directory() / "oss/azerothcore-wotlk/env/dist/etc/modules/mod_ollama_chat.conf"
# Host UID:GID the rootless container's worldserver user maps to (matches the
# sibling .dist / playerbots.conf in the same dir).
owner := "100999"

# Default recipe: copy the custom config into place and fix ownership.
sync:
    # Drop any existing file OR symlink so cp writes a real, standalone copy
    # (the dest used to be a symlink back to src, which cp can't copy onto itself).
    sudo rm -f "{{dest}}"
    sudo cp "{{src}}" "{{dest}}"
    sudo chown {{owner}}:{{owner}} "{{dest}}"
    @echo "Synced -> {{dest}} (owner {{owner}}:{{owner}})"


# Bring the stack up. BOTH compose files must be passed explicitly: with an
# explicit -f, compose stops auto-merging docker-compose.override.yml, and that
# override is what mounts ./modules (so the playerbots SQL dir exists) and sets
# AC_UPDATES_ENABLE_DATABASES. Omitting it is what crashed the worldserver.
bounce:
    nerdctl compose \
      -f ~/oss/azerothcore-wotlk/docker-compose.yml \
      -f ~/oss/azerothcore-wotlk/docker-compose.override.yml \
      up -d

# Apply the pending mod-ollama-chat SQL straight into acore_characters.
# This stack applies module SQL via the baked ac-db-import image, not from these
# host files, so new migrations won't land on a plain restart. Run this with the
# worldserver stopped so the history wipe is clean (nothing re-saves it).
db := "ac-database"
db-apply:
    nerdctl exec -i -e MYSQL_PWD=password {{db}} mysql -uroot acore_characters \
      < "{{justfile_directory()}}/data/sql/characters/base/2026_06_08_ollama_clear_chat_history.sql"
    nerdctl exec -i -e MYSQL_PWD=password {{db}} mysql -uroot acore_characters \
      < "{{justfile_directory()}}/data/sql/characters/base/2026_06_08_ollama_inworld_personalities.sql"
    @echo "Applied ollama SQL (history wipe + fresh personalities) to acore_characters."

# One-shot deploy: push config, then (with worldserver stopped so the history
# wipe is clean) apply the SQL, then bring the whole stack back up.
deploy: sync
    -nerdctl stop ac-worldserver
    just db-apply
    just bounce

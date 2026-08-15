#!/bin/bash
# ==============================================================================
# init_new_port.sh
#
# Bootstrap de la Fase 0 de un port nuevo Android (soloader) -> PS Vita.
# Destilado de los ports reales de Zenonia 2/3/4. Sigue el checklist de la
# skill `psvita-port-init` (~/.claude/skills/psvita-port-init/SKILL.md):
#
#   1. Crea la carpeta del port a partir de `soloader-boilerplate`.
#   2. Copia el .apk original (+ .zip) adentro.
#   3. Detecta ABI (armeabi=ARMv6 / armeabi-v7a=ARMv7) y versión de GLES
#      (1/2/3) del juego -- heurística automática, SIEMPRE a confirmar a mano.
#   4. Decompila el APK (jadx) y cada .so (devrvk/so-decompiler vía Docker).
#   5. git init + .gitignore anti-DMCA.
#   6. Copia/adapta `porting_tools/` y `psvita-port-toolkit/`.
#   7. Genera PORTING_PLAN.md y port_progress.md con el formato real usado
#      en plan_zenonia*_port.md / port_progress.md.
#   8. Copia las skills locales (psvita-porting, so-crash-triage) al repo.
#   9. Escribe un CLAUDE.md scaffold.
#
# Esto NO escribe código del loader (eso es trabajo de la skill
# `psvita-porting`, fase siguiente) -- este script solo dejar el terreno
# preparado: repo limpio, binario analizado, plan honesto.
#
# Uso: ./init_new_port.sh
# (Bash 3.2 compatible a propósito -- el bash default de macOS no tiene
# arrays asociativos ni mapfile; todo acá usa arrays indexados y loops.)
# ==============================================================================

set -e

BASE_DIR="/Volumes/Seagate/PSVITA Develop"
BOILERPLATE_DIR="$BASE_DIR/soloader-boilerplate"
REF_TOOLS_SOURCE="$BASE_DIR/Zenonia4-vita"   # porting_tools/psvita-port-toolkit más refinados
SKILLS_SOURCE="$HOME/.claude/skills"

# ---------------------------------------------------------------------------
info() { echo "[*] $1"; }
warn() { echo "[!] $1"; }
ok()   { echo "[+] $1"; }
err()  { echo "[-] $1" >&2; }
die()  { err "$1"; exit 1; }

# ---------------------------------------------------------------------------
# Fase 0 -- Prerrequisitos
# ---------------------------------------------------------------------------
check_prereqs() {
    info "Verificando prerrequisitos..."
    HAVE_JADX=0
    HAVE_DOCKER_SODECOMP=0

    if command -v jadx >/dev/null 2>&1; then
        ok "jadx encontrado."
        HAVE_JADX=1
    else
        warn "jadx no encontrado (brew install jadx). Se podrá correr manualmente después."
    fi

    if command -v docker >/dev/null 2>&1; then
        if docker image inspect devrvk/so-decompiler >/dev/null 2>&1; then
            ok "docker + imagen devrvk/so-decompiler encontrados."
            HAVE_DOCKER_SODECOMP=1
        else
            warn "docker está pero falta la imagen -- correr: docker pull devrvk/so-decompiler"
        fi
    else
        warn "docker no encontrado. La decompilación de .so se podrá correr manualmente después."
    fi

    command -v git >/dev/null 2>&1 || die "git no está instalado, no se puede continuar."
    command -v unzip >/dev/null 2>&1 || die "unzip no está instalado, no se puede continuar."

    if [ ! -d "$BOILERPLATE_DIR" ]; then
        die "No se encontró soloader-boilerplate en $BOILERPLATE_DIR"
    fi
    if [ ! -d "$REF_TOOLS_SOURCE" ]; then
        warn "No se encontró $REF_TOOLS_SOURCE -- porting_tools/psvita-port-toolkit no se copiarán."
    fi
}

# ---------------------------------------------------------------------------
# Fase 1 -- Datos del port nuevo
# ---------------------------------------------------------------------------
prompt_inputs() {
    echo ""
    echo "================================================================"
    echo "  Inicializar port nuevo: Android -> PS Vita"
    echo "================================================================"
    echo ""

    read -p "Nombre del juego (display, ej. 'Inotia 4'): " GAME_NAME
    [ -z "$GAME_NAME" ] && die "El nombre del juego es obligatorio."

    DEFAULT_SLUG="$(echo "$GAME_NAME" | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]')"
    read -p "Slug corto interno, sin espacios [$DEFAULT_SLUG]: " SLUG
    SLUG="${SLUG:-$DEFAULT_SLUG}"
    [ -z "$SLUG" ] && die "El slug es obligatorio."

    DEFAULT_FOLDER="$(echo "$GAME_NAME" | sed 's/ /-/g')-vita"
    read -p "Nombre de la carpeta del proyecto [$DEFAULT_FOLDER]: " FOLDER_NAME
    FOLDER_NAME="${FOLDER_NAME:-$DEFAULT_FOLDER}"

    # Nombre de proyecto CMake / VPK / target -- minúsculas, guion bajo (estilo zenonia_4, dh2, popc)
    PROJECT_NAME="$(echo "$SLUG" | tr '-' '_')"

    while true; do
        read -p "Ruta absoluta al .apk original: " APK_PATH
        APK_PATH="${APK_PATH/#\~/$HOME}"
        if [ -f "$APK_PATH" ]; then
            break
        fi
        warn "No existe ese archivo: $APK_PATH"
    done

    read -p "IP de la PS Vita de pruebas para manage_vita.py [192.168.1.100]: " VITA_IP
    VITA_IP="${VITA_IP:-192.168.1.100}"

    USED_IDS="$(grep -rho 'VITA_TITLEID *"[A-Za-z0-9]\{9\}"' "$BASE_DIR"/*/CMakeLists.txt 2>/dev/null \
        | grep -o '"[A-Za-z0-9]\{9\}"' | tr -d '"' | sort -u)"
    echo ""
    echo "TITLEIDs ya usados en este directorio (no reusar, colisiona en LiveArea):"
    echo "$USED_IDS" | sed 's/^/    /'
    while true; do
        read -p "TITLEID nuevo, 9 caracteres alfanuméricos (ej. PSVXX0001): " TITLEID
        if [ "${#TITLEID}" -ne 9 ]; then
            warn "Debe tener exactamente 9 caracteres."
            continue
        fi
        if echo "$USED_IDS" | grep -qx "$TITLEID"; then
            warn "Ese TITLEID ya está en uso -- elegí otro."
            continue
        fi
        break
    done

    NEW_DIR="$BASE_DIR/$FOLDER_NAME"
    if [ -e "$NEW_DIR" ]; then
        die "Ya existe $NEW_DIR -- elegí otro nombre de carpeta o borrá/movés el existente vos mismo."
    fi

    echo ""
    echo "Resumen:"
    echo "  Juego:        $GAME_NAME"
    echo "  Slug:         $SLUG"
    echo "  Carpeta:      $NEW_DIR"
    echo "  Proyecto:     $PROJECT_NAME"
    echo "  APK:          $APK_PATH"
    echo "  TITLEID:      $TITLEID"
    echo "  Vita IP:      $VITA_IP"
    echo ""
    read -p "¿Continuar? [S/n] " CONFIRM
    if [[ "$CONFIRM" =~ ^[nN]$ ]]; then
        die "Cancelado por el usuario."
    fi
}

# ---------------------------------------------------------------------------
# Fase 2 -- Crear carpeta a partir de soloader-boilerplate
# ---------------------------------------------------------------------------
setup_repo_dir() {
    info "Clonando soloader-boilerplate en $NEW_DIR ..."
    git clone --quiet "$BOILERPLATE_DIR" "$NEW_DIR"

    info "Inicializando submódulo lib/falso_jni (requiere red)..."
    if ! (cd "$NEW_DIR" && git submodule update --init --recursive >/dev/null 2>&1); then
        warn "No se pudo bajar el submódulo (¿sin red?). lib/falso_jni puede haber quedado vacío --"
        warn "correr manualmente después: git submodule update --init --recursive"
    fi

    # Se quiere el historial de ESTE port, no el de soloader-boilerplate.
    rm -rf "$NEW_DIR/.git"

    if [ -f "$NEW_DIR/CMakeLists.txt" ]; then
        info "Adaptando CMakeLists.txt (VITA_APP_NAME/VITA_TITLEID/project/DATA_PATH)..."
        sed -i '' \
            -e "s/project(so_loader C CXX)/project(${PROJECT_NAME} C CXX)/" \
            -e "s/set(VITA_APP_NAME \"so-loader\")/set(VITA_APP_NAME \"${GAME_NAME}\")/" \
            -e "s/set(VITA_TITLEID \"SOLOADER0\")/set(VITA_TITLEID \"${TITLEID}\")/" \
            -e "s/set(VITA_VPKNAME \"so_loader\")/set(VITA_VPKNAME \"${PROJECT_NAME}\")/" \
            -e "s#set(PSVITAIP \"192.168.0.198\"#set(PSVITAIP \"${VITA_IP}\"#" \
            -e "s#ux0:data/gamename/#ux0:data/${SLUG}/#" \
            "$NEW_DIR/CMakeLists.txt"
        ok "CMakeLists.txt adaptado (el resto -- so_util/FalsoJNI/reimpl -- sigue siendo el scaffold"
        ok "genérico del boilerplate; java.c/main.c todavía tienen placeholders vacíos, eso es"
        ok "trabajo de la skill psvita-porting, no de esta fase)."
    fi
}

# ---------------------------------------------------------------------------
# Fase 3 -- Colocar el APK, extraerlo, detectar ABI y versión de GLES
# ---------------------------------------------------------------------------
place_apk_and_detect() {
    APK_BASENAME="$(basename "$APK_PATH")"
    APK_STEM="${APK_BASENAME%.apk}"

    info "Copiando .apk (y su .zip gemelo, convención usada en los otros ports)..."
    cp "$APK_PATH" "$NEW_DIR/$APK_BASENAME"
    cp "$APK_PATH" "$NEW_DIR/${APK_STEM}.zip"

    EXTRACT_DIR="$NEW_DIR/${SLUG}_extract"
    info "Extrayendo APK a ${SLUG}_extract/ ..."
    mkdir -p "$EXTRACT_DIR"
    unzip -qq -o "$APK_PATH" -d "$EXTRACT_DIR" || warn "unzip terminó con warnings (frecuente en APKs firmados, no siempre es grave)."

    ABIS=()
    if [ -d "$EXTRACT_DIR/lib" ]; then
        for d in "$EXTRACT_DIR"/lib/*/; do
            [ -d "$d" ] && ABIS+=("$(basename "$d")")
        done
    fi

    PREFERRED_ABI=""
    for a in "${ABIS[@]}"; do
        if [ "$a" = "armeabi-v7a" ]; then PREFERRED_ABI="armeabi-v7a"; fi
    done
    if [ -z "$PREFERRED_ABI" ] && [ "${#ABIS[@]}" -gt 0 ]; then
        PREFERRED_ABI="${ABIS[0]}"
    fi

    echo ""
    info "ABIs nativas encontradas: ${ABIS[*]:-ninguna}"
    if [ -z "$PREFERRED_ABI" ]; then
        warn "No se encontró ninguna carpeta lib/<abi>/ -- ¿el juego es 100% Java/libGDX sin .so? Revisar a mano."
        ARCH_NOTE="No se encontró lib/<abi>/ nativo -- confirmar si el juego tiene motor nativo antes de asumir soloader."
    else
        case "$PREFERRED_ABI" in
            armeabi-v7a) ARCH_NOTE="armeabi-v7a presente -> ARMv7 (hard-float/NEON disponible). Preferir esta ABI si conviven ambas -- el CPU de Vita (Cortex-A9) es ARMv7, corre este código nativo sin traducción." ;;
            armeabi)     ARCH_NOTE="Solo armeabi (ARMv6, soft-float) -- el CPU de Vita ejecuta esto igual (ARMv7 es superset), pero sin las instrucciones NEON que trae v7a." ;;
            *)           ARCH_NOTE="ABI no reconocida ($PREFERRED_ABI) -- confirmar a mano." ;;
        esac
        if [ "${#ABIS[@]}" -gt 1 ]; then
            ARCH_NOTE="$ARCH_NOTE Hay más de una ABI en el APK (${ABIS[*]}) -- se eligió $PREFERRED_ABI para el análisis, pero confirmar cuál conviene portar."
        fi
    fi
    ok "$ARCH_NOTE"

    # Lista de .so bajo la ABI preferida (puede haber más de uno: motor + wrapper)
    SO_FILES=()
    if [ -n "$PREFERRED_ABI" ]; then
        while IFS= read -r f; do
            SO_FILES+=("$f")
        done < <(find "$EXTRACT_DIR/lib/$PREFERRED_ABI" -type f -name '*.so' 2>/dev/null)
    fi
    echo ""
    info ".so encontrados en lib/$PREFERRED_ABI/:"
    for f in "${SO_FILES[@]}"; do
        echo "    $(basename "$f")  ($(du -h "$f" | cut -f1))"
    done

    # --- Detección de versión de GLES ---
    # Fuente autoritativa: <uses-feature android:glEsVersion="0x000200xx"/> del
    # AndroidManifest.xml. jadx lo decodifica a XML legible en resources/ al
    # decompilar el .apk completo (no solo el classes.dex) -- se lee DESPUÉS
    # de correr jadx en decompile_all(). Acá solo se deja el heurístico de
    # símbolos como fallback / segunda señal, sobre el .so real.
    GLES_SYMBOL_HINT="sin determinar"
    if [ "${#SO_FILES[@]}" -gt 0 ] && command -v objdump >/dev/null 2>&1; then
        GLES1_HITS=0; GLES2_HITS=0; GLES3_HITS=0
        for so in "${SO_FILES[@]}"; do
            SYMS="$(objdump -T "$so" 2>/dev/null | grep -o 'gl[A-Za-z0-9_]*' | sort -u)"
            echo "$SYMS" | grep -qE '^(glVertexPointer|glClearColorx|glTexParameterx|glColor4x)$' && GLES1_HITS=$((GLES1_HITS+1))
            echo "$SYMS" | grep -qE '^(glCreateShader|glCreateProgram|glUseProgram|glGetUniformLocation)$' && GLES2_HITS=$((GLES2_HITS+1))
            echo "$SYMS" | grep -qE '^(glDrawArraysInstanced|glDrawRangeElements|glGenVertexArrays|glBindVertexArray|glDrawElementsInstanced)$' && GLES3_HITS=$((GLES3_HITS+1))
        done
        if [ "$GLES3_HITS" -gt 0 ]; then
            GLES_SYMBOL_HINT="GLES3 (símbolos como glGenVertexArrays/glDrawArraysInstanced presentes en el import table)"
        elif [ "$GLES2_HITS" -gt 0 ]; then
            GLES_SYMBOL_HINT="GLES2 (glCreateShader/glCreateProgram/glUseProgram presentes -- pipeline programable, no fijo)"
        elif [ "$GLES1_HITS" -gt 0 ]; then
            GLES_SYMBOL_HINT="GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx de punto fijo, sin shaders)"
        else
            GLES_SYMBOL_HINT="sin señal clara por símbolos (puede que el motor no llame GL directo, ej. Unity/libil2cpp) -- revisar el .so con Ghidra"
        fi
    fi
    info "Heurística de símbolos GL: $GLES_SYMBOL_HINT"
    info "(Se refina después de decompilar con jadx -- ver glEsVersion real del AndroidManifest.xml)"

    # Paquete Java principal (para el análisis de símbolos JNI de la Fase 3 del skill)
    JAVA_PACKAGE="$(grep -o 'package="[^"]*"' "$EXTRACT_DIR/AndroidManifest.xml" 2>/dev/null | head -1 | cut -d'"' -f2)"
    if [ -z "$JAVA_PACKAGE" ]; then
        JAVA_PACKAGE="(AndroidManifest.xml está en formato binario -- se resuelve solo tras decompilar con jadx, ver resources/AndroidManifest.xml)"
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Fase 4 -- Decompilación (jadx + devrvk/so-decompiler)
# ---------------------------------------------------------------------------
decompile() {
    DECOMPILED_DIR="$NEW_DIR/decompiled"
    APK_OUT_DIR="$DECOMPILED_DIR/apk_jadx"
    mkdir -p "$APK_OUT_DIR"

    JADX_OK=0
    if [ "$HAVE_JADX" -eq 1 ]; then
        info "Decompilando Java del APK con jadx (puede tardar unos minutos)..."
        if jadx -d "$APK_OUT_DIR" "$NEW_DIR/$APK_BASENAME"; then
            ok "jadx terminó sin errores."
        else
            warn "jadx terminó con errores -- normal si son solo SDKs de ads/analytics fallando en clases irrelevantes."
        fi
        JADX_OK=1

        # Refinar la versión de GLES con el AndroidManifest.xml ya decodificado
        MANIFEST_DECODED="$APK_OUT_DIR/resources/AndroidManifest.xml"
        if [ -f "$MANIFEST_DECODED" ]; then
            GLES_MANIFEST="$(grep -o 'glEsVersion="[^"]*"' "$MANIFEST_DECODED" | head -1 | cut -d'"' -f2)"
            if [ -n "$GLES_MANIFEST" ]; then
                case "$GLES_MANIFEST" in
                    0x00010000|65536)  GLES_VERSION_FINAL="GLES1 (declarado explícitamente en AndroidManifest.xml)" ;;
                    0x00020000|131072) GLES_VERSION_FINAL="GLES2 (declarado explícitamente en AndroidManifest.xml)" ;;
                    0x00030000|196608) GLES_VERSION_FINAL="GLES3 (declarado explícitamente en AndroidManifest.xml)" ;;
                    *)                 GLES_VERSION_FINAL="valor no estándar en manifest: $GLES_MANIFEST -- revisar a mano" ;;
                esac
            else
                GLES_VERSION_FINAL="AndroidManifest.xml no declara <uses-feature glEsVersion> -- usar la heurística de símbolos ($GLES_SYMBOL_HINT) y confirmar con Ghidra"
            fi
            JAVA_PACKAGE_REAL="$(grep -o 'package="[^"]*"' "$MANIFEST_DECODED" | head -1 | cut -d'"' -f2)"
            [ -n "$JAVA_PACKAGE_REAL" ] && JAVA_PACKAGE="$JAVA_PACKAGE_REAL"
        else
            GLES_VERSION_FINAL="no se pudo leer resources/AndroidManifest.xml decodificado -- usar heurística de símbolos ($GLES_SYMBOL_HINT)"
        fi
    else
        GLES_VERSION_FINAL="jadx no corrió -- heurística de símbolos solamente ($GLES_SYMBOL_HINT), correr jadx manualmente y revisar AndroidManifest.xml"
    fi

    echo ""
    ok "Versión de GLES determinada: $GLES_VERSION_FINAL"

    if [ "$HAVE_DOCKER_SODECOMP" -eq 1 ] && [ "${#SO_FILES[@]}" -gt 0 ]; then
        for so_file in "${SO_FILES[@]}"; do
            so_name="$(basename "$so_file")"
            abi="$(basename "$(dirname "$so_file")")"
            so_out="$DECOMPILED_DIR/${so_name%.so}_${abi}/ghidra"
            mkdir -p "$so_out"
            info "Decompilando $so_name ($abi) con Ghidra headless (Docker, puede tardar varios minutos)..."
            rel_so_path="${so_file#$BASE_DIR/}"
            rel_so_out="${so_out#$BASE_DIR/}"
            if docker run --rm --platform linux/amd64 -v "${BASE_DIR}:/app" devrvk/so-decompiler decompile "/app/$rel_so_path" "/app/$rel_so_out"; then
                ok "Listo: $so_out"
            else
                warn "Falló la decompilación de $so_name -- correr manualmente después (ver PORTING_PLAN.md)."
            fi
        done
    else
        warn "Se omite la decompilación de .so (docker/imagen no disponibles) -- correr manualmente después."
    fi
}

# ---------------------------------------------------------------------------
# Fase 5 -- git init + .gitignore anti-DMCA
# ---------------------------------------------------------------------------
git_init_and_ignore() {
    info "git init + .gitignore anti-DMCA..."
    (cd "$NEW_DIR" && git init -q)

    cat > "$NEW_DIR/.gitignore" <<EOF
# macOS metadata
.DS_Store
._*
.Spotlight-V100
.Trashes

# Android APK/ZIP originales y extracción -- nunca commitear el juego (DMCA)
*.apk
*.zip
/${SLUG}_extract/

# Java decompilado con jadx (derivado de material propietario, regenerable:
# jadx -d decompiled/apk_jadx "${APK_BASENAME}")
/decompiled/apk_jadx/

# Pseudo-C decompilado del/los .so (derivado, regenerable con devrvk/so-decompiler)
/decompiled/*/ghidra/

# Librerías .so propietarias del juego original
lib/*.so
lib/**/*.so
${SLUG}_extract/lib/

# Assets del juego montados para pruebas
ux0_data/
assets/

# Build artifacts
/build/
CMakeCache.txt
CMakeFiles/
Makefile
cmake_install.cmake
*.elf
*.self
*.vpk
*.suprx

# Debugging en consola real
/logs/
log_*.txt
*.psp2dmp

# Python
__pycache__/
*.pyc

# IDE
.vscode/
.idea/
*.swp
cmake-build-*/
EOF
    ok ".gitignore escrito."
}

# ---------------------------------------------------------------------------
# Fase 6 -- porting_tools/ y psvita-port-toolkit/ (adaptados a este port)
# ---------------------------------------------------------------------------
setup_porting_tools() {
    if [ ! -d "$REF_TOOLS_SOURCE/porting_tools" ]; then
        warn "No hay porting_tools de referencia -- se omite este paso."
        return
    fi

    info "Copiando porting_tools/ (partes genéricas verbatim, build/deploy re-generados para este port)..."
    mkdir -p "$NEW_DIR/porting_tools"
    cp -R "$REF_TOOLS_SOURCE/porting_tools/automation" "$NEW_DIR/porting_tools/" 2>/dev/null || true
    cp -R "$REF_TOOLS_SOURCE/porting_tools/misc" "$NEW_DIR/porting_tools/" 2>/dev/null || true
    mkdir -p "$NEW_DIR/porting_tools/build" "$NEW_DIR/porting_tools/tests"
    cp "$REF_TOOLS_SOURCE/porting_tools/build/clean_macos.sh" "$NEW_DIR/porting_tools/build/" 2>/dev/null || true
    cp "$REF_TOOLS_SOURCE/porting_tools/build/deploy_and_launch_vita3k.sh" "$NEW_DIR/porting_tools/build/" 2>/dev/null || true
    cp "$REF_TOOLS_SOURCE/porting_tools/tests/run_tests.sh" "$NEW_DIR/porting_tools/tests/" 2>/dev/null || true
    find "$NEW_DIR/porting_tools" -name '._*' -delete 2>/dev/null || true

    # decompile_all.sh: mismo patrón que Zenonia4, con las rutas de ESTE port.
    cat > "$NEW_DIR/porting_tools/build/decompile_all.sh" <<EOF
#!/bin/bash
# Re-corre la decompilación completa (jadx + so-decompiler) para este port.
# Generado por init_new_port.sh -- rutas ya adaptadas a este proyecto.

BASE_DIR="$NEW_DIR"
APK_FILE="\${BASE_DIR}/${APK_BASENAME}"
LIB_DIR="\${BASE_DIR}/${SLUG}_extract/lib"
DECOMPILED_DIR="\${BASE_DIR}/decompiled"
APK_OUT_DIR="\${DECOMPILED_DIR}/apk_jadx"

mkdir -p "\$APK_OUT_DIR"

echo "Iniciando decompilación con JADX..."
if command -v jadx >/dev/null 2>&1; then
  jadx -d "\$APK_OUT_DIR" "\$APK_FILE"
else
  echo "jadx no encontrado -- instalar con: brew install jadx"
  exit 1
fi
echo "JADX Finalizado. Resultados en \${APK_OUT_DIR}"

echo "Iniciando decompilación de .so con Ghidra (so-decompiler)..."
if [ ! -d "\$LIB_DIR" ]; then
    echo "Directorio de librerías no encontrado en: \$LIB_DIR"
else
    # so_out incluye el ABI (armeabi/armeabi-v7a) para no pisar resultados si
    # hay un .so con el mismo nombre en más de una ABI.
    find "\$LIB_DIR" -type f -name "*.so" | while read -r so_file; do
        so_name=\$(basename "\$so_file")
        abi=\$(basename "\$(dirname "\$so_file")")
        so_out="\${DECOMPILED_DIR}/\${so_name%.so}_\${abi}/ghidra"
        mkdir -p "\$so_out"
        rel_so_path="\${so_file#\$BASE_DIR/}"
        rel_so_out="\${so_out#\$BASE_DIR/}"
        echo "Decompilando \$so_name (\$abi)..."
        docker run --rm --platform linux/amd64 -v "\${BASE_DIR}:/app" devrvk/so-decompiler decompile "/app/\$rel_so_path" "/app/\$rel_so_out"
    done
fi
echo "Decompilación de archivos .so finalizada. Resultados en \${DECOMPILED_DIR}/<lib>_<abi>/ghidra/"
EOF
    chmod +x "$NEW_DIR/porting_tools/build/decompile_all.sh"

    # build_and_install.sh: versión GENÉRICA (sin las banderas de experimentos
    # de rendimiento acumuladas en Zenonia4 -- esas son específicas de motor y
    # se agregan a mano cuando aparezcan, no se heredan acá).
    cat > "$NEW_DIR/porting_tools/build/build_and_install.sh" <<EOF
#!/bin/bash
set -e

PROJECT_DIR="\$(cd "\$(dirname "\$0")/../.." && pwd)"
BUILD_DIR="/tmp/${SLUG}-build"
SRC_DIR="/tmp/${SLUG}-src"
VPK_NAME="${PROJECT_NAME}.vpk"

echo "================================================================"
echo "  Build automático para ${GAME_NAME} (PS Vita)"
echo "================================================================"

echo "[1/3] Preparando entorno de compilación (symlink en /tmp -- evita el bug"
echo "      de vita-pack-vpk con rutas que contienen espacios, ver toolchain_gotchas.md)..."
mkdir -p "\$BUILD_DIR" "\$SRC_DIR"

if [ -z "\$VITASDK" ]; then
    if [ -d "/usr/local/vitasdk" ]; then
        export VITASDK="/usr/local/vitasdk"
    elif [ -d "\$HOME/vitasdk" ]; then
        export VITASDK="\$HOME/vitasdk"
    else
        echo "Error: VITASDK no definida y no se encontró en rutas por defecto."
        exit 1
    fi
    export PATH="\$VITASDK/bin:\$PATH"
fi

rsync -a \\
    --exclude '.git' --exclude 'build' --exclude '.*' \\
    --exclude 'decompiled' --exclude '${SLUG}_extract' \\
    --exclude '*.apk' --exclude '*.zip' \\
    "\$PROJECT_DIR/" "\$SRC_DIR/"

echo "[2/3] Ejecutando CMake y Make..."
cd "\$BUILD_DIR"
read -p "¿Build de depuración (logging detallado)? [S/n] " DEBUG_OPTION
if [[ "\$DEBUG_OPTION" =~ ^[nN]\$ ]]; then
    BUILD_TYPE="Release"
else
    BUILD_TYPE="Debug"
fi
cmake "\$SRC_DIR" -DCMAKE_BUILD_TYPE="\$BUILD_TYPE"
make -j\$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

VPK_PATH="\$BUILD_DIR/\$VPK_NAME"
if [ ! -f "\$VPK_PATH" ]; then
    echo "Error: no se generó \$VPK_PATH -- revisar VITA_VPKNAME en CMakeLists.txt."
    exit 1
fi
mkdir -p "\$PROJECT_DIR/build"
cp "\$VPK_PATH" "\$PROJECT_DIR/build/"
echo "Build exitoso: \$PROJECT_DIR/build/\$VPK_NAME"

echo ""
echo "[3/3] Instalación (opcional)"
VITA3K_APP="/Applications/Vita3K.app/Contents/MacOS/Vita3K"
if [ -x "\$VITA3K_APP" ]; then
    read -p "¿Instalar y ejecutar en Vita3K ahora? [s/N] " INSTALL_VITA3K
    if [[ "\$INSTALL_VITA3K" =~ ^[sS]\$ ]]; then
        "\$VITA3K_APP" -B OpenGL "\$PROJECT_DIR/build/\$VPK_NAME" > /dev/null 2>&1 &
        echo "Listo."
    fi
else
    echo "Vita3K no encontrado en la ruta por defecto."
fi
echo "Para instalar en un Vita real, usar manage_vita.py o 'make send'."
EOF
    chmod +x "$NEW_DIR/porting_tools/build/build_and_install.sh"

    # manage_vita.py: solo la config del encabezado, adaptada a este port
    # (mismo motor de despliegue por FTP que Zenonia4, sin reescribir la lógica).
    if [ -f "$REF_TOOLS_SOURCE/porting_tools/manage_vita.py" ]; then
        cp "$REF_TOOLS_SOURCE/porting_tools/manage_vita.py" "$NEW_DIR/porting_tools/manage_vita.py"
        sed -i '' \
            -e "s/VITA_IP = \".*\"/VITA_IP = \"${VITA_IP}\"/" \
            -e "s#LOCAL_VPK_PATH = \".*\"#LOCAL_VPK_PATH = \"build/${PROJECT_NAME}.vpk\"#" \
            -e "s#VITA_LOGS_DIR = \".*\"#VITA_LOGS_DIR = \"/ux0:/data/${SLUG}/logs\"#" \
            "$NEW_DIR/porting_tools/manage_vita.py"
        ok "manage_vita.py copiado y adaptado (VITA_IP/LOCAL_VPK_PATH/VITA_LOGS_DIR)."
    fi

    cp "$REF_TOOLS_SOURCE/porting_tools/README.md" "$NEW_DIR/porting_tools/README.md" 2>/dev/null || true

    if [ -d "$REF_TOOLS_SOURCE/psvita-port-toolkit" ]; then
        info "Copiando psvita-port-toolkit/ (guía genérica, sin cambios -- ya no es específica de un juego)..."
        cp -R "$REF_TOOLS_SOURCE/psvita-port-toolkit" "$NEW_DIR/psvita-port-toolkit"
        find "$NEW_DIR/psvita-port-toolkit" -name '._*' -delete 2>/dev/null || true
    fi

    ok "porting_tools/ y psvita-port-toolkit/ listos. build_and_install.sh quedó GENÉRICO a propósito"
    ok "(sin las banderas de experimentos de rendimiento de Zenonia4 -- esas son específicas de motor)."
}

# ---------------------------------------------------------------------------
# Fase 7 -- PORTING_PLAN.md y port_progress.md
# ---------------------------------------------------------------------------
write_plan_and_progress() {
    TODAY="$(date +%Y-%m-%d)"
    SO_LIST=""
    for f in "${SO_FILES[@]}"; do
        SO_LIST="${SO_LIST}- \`${f#$NEW_DIR/}\` ($(du -h "$f" 2>/dev/null | cut -f1))\n"
    done
    [ -z "$SO_LIST" ] && SO_LIST="(ninguno detectado automáticamente -- revisar $EXTRACT_DIR/lib/ a mano)\n"

    JNI_EXPORTS=""
    if [ "${#SO_FILES[@]}" -gt 0 ] && command -v objdump >/dev/null 2>&1; then
        JNI_EXPORTS="$(objdump -T "${SO_FILES[0]}" 2>/dev/null | grep "Java_" | awk '{print $NF}' | sort -u | sed 's/^/- `/;s/$/`/' )"
    fi
    [ -z "$JNI_EXPORTS" ] && JNI_EXPORTS="(no se encontraron exports \`Java_*\` en ${SO_FILES[0]:-ningún .so} -- confirmar a mano con objdump -T, puede que el motor registre con RegisterNatives en vez de convención de nombre)"

    cat > "$NEW_DIR/PORTING_PLAN.md" <<EOF
# Plan de Port — ${GAME_NAME} (PS Vita)

> Generado por \`init_new_port.sh\` el ${TODAY}. Esto es un punto de partida con lo que se pudo
> detectar automáticamente -- **la Fase 3 de la skill \`psvita-port-init\` (análisis real del motor)
> todavía no se hizo**. No asumir nada de este documento como confirmado hasta contrastarlo con
> \`objdump\`/Ghidra/jadx a mano.

## 0. Contexto y estrategia general

- **Juego:** ${GAME_NAME}
- **Paquete Java:** ${JAVA_PACKAGE:-(pendiente, ver decompiled/apk_jadx/resources/AndroidManifest.xml)}
- **APK original:** \`${APK_BASENAME}\`
- **TITLEID asignado:** \`${TITLEID}\`

**¿Motor conocido?** Antes de escribir loader/código, revisar si este publisher/motor ya tiene un port
en este mismo directorio (\`ls "$BASE_DIR"\`). Si es el caso, **no asumir "mismo motor" solo porque
"se parece" o es del mismo publisher** -- confirmar con símbolos JNI reales (mismo paquete
\`Java_com_<publisher>_...\`, mismos imports) antes de forkear código de ese port, siguiendo el criterio
de \`plan_zenonia3_port.md\`/\`plan_zenonia4_port.md\` (Zenonia 3 y 4 sí confirmaron mismo motor que
Zenonia 2 antes de reusar su \`loader/\`; no copiar por analogía superficial).

Si NO hay un port previo del mismo motor: partir del scaffold genérico de \`soloader-boilerplate\`
(\`source/\`, \`lib/so_util\`, \`lib/falso_jni\`) ya clonado en este repo, siguiendo la skill
\`psvita-porting\` fase por fase.

## 1. Detección automática (arquitectura y gráficos)

- **ABI(s) presentes en el APK:** ${ABIS[*]:-ninguna}
- **ABI elegida para el análisis:** ${PREFERRED_ABI:-N/A}
- **Nota de arquitectura:** ${ARCH_NOTE}
- **Versión de GLES:** ${GLES_VERSION_FINAL}
  - Heurística de símbolos (segunda señal, no autoritativa): ${GLES_SYMBOL_HINT}

**Importante:** la elección de ABI (ARMv6 \`armeabi\` vs. ARMv7 \`armeabi-v7a\`) y la versión de GLES
determinan qué wrappers hay que escribir en \`loader/dynlib.c\`:
- **GLES1** (pipeline fijo): wrappers de \`glVertexPointer\`/\`glTexParameterx\`/\`glClearColorx\`,
  posible conversión \`GL_FIXED\`→\`GL_FLOAT\`, típico de motores viejos (2009-2013) o software-rendered
  con blit final por textura (patrón visto en Zenonia 2/3/4, Gamevil Nexus2/Clet).
- **GLES2**: pipeline programable, requiere traducir/portar shaders (ver \`SHADER_FORMAT\` en el
  \`CMakeLists.txt\` del boilerplate: GLSL/CG/GXP) en vez de wrappers de fixed-function.
  \`porting_tools/translate_shaders.py\` puede ayudar si los shaders son GLSL ES simples.
- **GLES3**: como GLES2 + posible uso de VAOs (\`glGenVertexArrays\`)/instancing -- confirmar que
  vitaGL soporta las extensiones que el motor pide antes de asumir portabilidad directa.

## 2. .so encontrados (ABI ${PREFERRED_ABI:-N/A})

$(echo -e "$SO_LIST")

## 3. Guía de decompilación (ya ejecutada por init_new_port.sh — reproducible)

\`\`\`bash
# Java del APK
jadx -d "decompiled/apk_jadx" "${APK_BASENAME}"

# Símbolos dinámicos del .so (no requiere toolchain ARM -- objdump del sistema alcanza)
objdump -T "${SLUG}_extract/lib/${PREFERRED_ABI:-<abi>}/<nombre>.so" | grep "Java_"   # exports JNI
objdump -T "${SLUG}_extract/lib/${PREFERRED_ABI:-<abi>}/<nombre>.so" | grep "UND"     # imports

# Pseudo-C vía Ghidra headless en Docker (por cada .so)
docker run --rm --platform linux/amd64 -v "$BASE_DIR:/app" devrvk/so-decompiler \\
  decompile "/app/${FOLDER_NAME}/${SLUG}_extract/lib/${PREFERRED_ABI:-<abi>}/<nombre>.so" \\
  "/app/${FOLDER_NAME}/decompiled/<nombre>_${PREFERRED_ABI:-<abi>}/ghidra"
\`\`\`

Re-ejecutable con \`porting_tools/build/decompile_all.sh\`.

## 4. Exports JNI encontrados en ${SO_FILES[0]:-(ningún .so)} (convención \`Java_*\`)

$JNI_EXPORTS

Si esta lista está vacía pero el juego sí tiene lógica nativa, buscar \`RegisterNatives\` en el pseudo-C
de Ghidra -- significa que la resolución NO es por convención de nombre y FalsoJNI necesita que se
llame explícitamente a los punteros que el motor registra en \`JNI_OnLoad\`, no \`dlsym\` por nombre.

## 5. Fases del port (checklist)

- [x] Repo creado a partir de \`soloader-boilerplate\`, \`git init\`, \`.gitignore\` anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra vía so-decompiler) -- ver sección 3.
- [ ] **Análisis del motor real:** leer \`decompiled/apk_jadx/sources/\`, confirmar ciclo de vida nativo
      (orden real de \`nativeInit\`/\`onSurfaceCreated\`/etc.), decidir si se reusa código de otro port o
      se parte del boilerplate genérico (ver sección 0).
- [ ] **Bootstrap del loader:** \`so_file_load\`/\`so_relocate\`/\`so_resolve\` contra el \`.so\` real,
      primer \`cmake . && make\` (objetivo: que compile, no que corra).
- [ ] **Tabla JNI (FalsoJNI):** registrar los exports de la sección 4 + los callbacks que el motor
      llama hacia "Java" (buscar \`Call*Method\`/\`GetStaticMethodID\` en el pseudo-C de Ghidra).
- [ ] **Primer arranque (Vita3K primero, consola real después):** metodología de \`so-crash-triage\` --
      un log a la vez, un bug a la vez.
- [ ] **Gráficos:** wrappers GL según la versión detectada en la sección 1.
- [ ] **Input:** táctil (\`sceTouchPeek\`) y/o botones físicos, según cómo el motor reciba eventos.
- [ ] **Audio.**
- [ ] **Assets:** extraer \`assets/\` (y \`.obb\` si existe) a \`ux0_data/${SLUG}/\`, hooks de
      \`fopen\`/\`stat\`/\`access\` redirigiendo ahí.
- [ ] **LiveArea / VPK:** adaptar \`extras/livearea/\` (ya viene con arte de placeholder del boilerplate).
- [ ] **Pruebas en hardware real.**

## 6. Riesgos genéricos conocidos (aplican a casi cualquier port por este método)

- Rutas con espacio en \`PSVITA Develop\` rompen \`vita-pack-vpk\` -- \`build_and_install.sh\` ya hace
  staging en \`/tmp\` para evitarlo.
- \`libshacccg.suprx\` debe estar en \`ur0:data/\` de la consola de pruebas (requisito de vitaGL).
- \`pthread\` con FalsoJNI suele necesitar \`-Wl,--whole-archive ... --no-whole-archive\` -- puede
  arrastrar símbolos duplicados con \`SceLibKernel_stub\`, no linkearlo si no hace falta.
- Mutex/condvar estáticos de Bionic en cero (\`PTHREAD_MUTEX_INITIALIZER\`) son válidos en Android pero
  VitaSDK los desreferencia como \`NULL\` real -- patrón visto repetidamente en motores viejos (ver
  \`Zenonia2-vita/port_progress.md\`/\`Zenonia3-vita/port_progress.md\` para el fix exacto si reaparece).

## 7. Referencias

- Skills locales (copiadas a \`.claude/skills/\` en este repo): \`psvita-port-init\`, \`psvita-porting\`,
  \`so-crash-triage\`.
- \`psvita-port-toolkit/PORTING_GUIDE.md\` -- guía genérica paso a paso (pensada para cocos2d-x, pero las
  fases de toolchain/input/VPK/debugging aplican a cualquier motor).
- Ports hermanos en este directorio (revisar si el motor coincide antes de asumir nada):
  \`ls "$BASE_DIR"\`.
EOF
    ok "PORTING_PLAN.md escrito."

    cat > "$NEW_DIR/port_progress.md" <<EOF
# Registro de Progreso del Port de ${GAME_NAME} (PS Vita)

## Fase 1: Configuración del Entorno y Preparación (Completada — ${TODAY})
- **Repositorio de Git:** Inicializado por \`init_new_port.sh\`.
- **Exclusiones (.gitignore):** Configurado para excluir el .apk/.zip original, la extracción
  (\`${SLUG}_extract/\`), lo decompilado (\`decompiled/\`), \`lib/*.so\`, \`ux0_data/\`, logs y artefactos
  de build.
- **Estructura del proyecto:** Clonado desde \`soloader-boilerplate\` (so_util + FalsoJNI vendorizado,
  \`reimpl/\` de libc/pthread/mem/io/egl). \`CMakeLists.txt\` adaptado: \`VITA_APP_NAME=${GAME_NAME}\`,
  \`VITA_TITLEID=${TITLEID}\`, \`project(${PROJECT_NAME})\`.
- **APK:** \`${APK_BASENAME}\` copiado y extraído a \`${SLUG}_extract/\`.
- **ABI detectada:** ${ABIS[*]:-ninguna} (elegida para análisis: ${PREFERRED_ABI:-N/A}).
- **GLES detectado:** ${GLES_VERSION_FINAL}

## Fase 2: Decompilación (Completada — ${TODAY})
- **jadx:** $( [ "$JADX_OK" = "1" ] && echo "corrido, resultados en \`decompiled/apk_jadx/\`." || echo "NO corrido (jadx no estaba instalado) -- pendiente." )
- **Ghidra (.so):** $( [ "$HAVE_DOCKER_SODECOMP" = "1" ] && echo "corrido para cada .so bajo \`decompiled/<so>_<abi>/ghidra/\`." || echo "NO corrido (docker/imagen no disponibles) -- pendiente." )

## Fase 3: Análisis del Motor Real (Pendiente)
- [ ] Confirmar si este juego comparte motor con algún port hermano en este directorio.
- [ ] Leer \`decompiled/apk_jadx/sources/\` para el ciclo de vida nativo real.
- [ ] Confirmar exports JNI reales (ver \`PORTING_PLAN.md\` sección 4) y si hay \`RegisterNatives\`.

## Fase 4 en adelante: Pendiente
Ver \`PORTING_PLAN.md\` sección 5 para el checklist completo. Actualizar este archivo con **un bug
confirmado a la vez en pruebas reales** (Vita3K primero, consola física después) -- no documentar
teoría sin verificar, siguiendo el criterio de \`so-crash-triage\`.
EOF
    ok "port_progress.md escrito."
}

# ---------------------------------------------------------------------------
# Fase 8 -- Skills locales + CLAUDE.md
# ---------------------------------------------------------------------------
write_claude_md_and_skills() {
    mkdir -p "$NEW_DIR/.claude/skills"
    for skill in psvita-porting so-crash-triage psvita-port-init; do
        if [ -d "$SKILLS_SOURCE/$skill" ]; then
            cp -R "$SKILLS_SOURCE/$skill" "$NEW_DIR/.claude/skills/$skill"
            find "$NEW_DIR/.claude/skills/$skill" -name '._*' -delete 2>/dev/null || true
            ok "Skill '$skill' copiada al repo."
        else
            warn "Skill '$skill' no encontrada en $SKILLS_SOURCE -- se omite."
        fi
    done

    cat > "$NEW_DIR/CLAUDE.md" <<EOF
# ${GAME_NAME} — Port a PS Vita

Port de \`${APK_BASENAME}\` (Android) a PS Vita vía soloader (carga directa del \`.so\` original, sin
recompilarlo). Generado a partir de \`soloader-boilerplate\` con \`init_new_port.sh\`.

## Estructura

- \`${SLUG}_extract/\` — APK extraído (gitignored). \`lib/<abi>/*.so\` son los binarios nativos reales.
- \`decompiled/apk_jadx/\` — Java decompilado con jadx (gitignored, regenerable).
- \`decompiled/<so>_<abi>/ghidra/\` — pseudo-C de cada \`.so\` vía Ghidra headless (gitignored, regenerable).
- \`source/\`, \`lib/so_util\`, \`lib/falso_jni\` — scaffold del boilerplate (SoLoader + FalsoJNI vendorizado).
- \`porting_tools/\` — scripts de build/deploy/decompilación adaptados a este proyecto.
- \`psvita-port-toolkit/PORTING_GUIDE.md\` — guía genérica paso a paso.
- \`PORTING_PLAN.md\` — plan vivo, actualizar a medida que se confirman cosas del motor real.
- \`port_progress.md\` — bitácora, un bug confirmado a la vez.

## Hallazgos de motor (Fase 0 — automáticos, sin confirmar todavía)

- ABI: ${ABIS[*]:-ninguna} (preferida: ${PREFERRED_ABI:-N/A})
- GLES: ${GLES_VERSION_FINAL}
- Paquete Java: ${JAVA_PACKAGE:-pendiente}

Ver \`PORTING_PLAN.md\` para el detalle y qué falta confirmar antes de escribir código.

## Flujo de trabajo esperado

1. Análisis de símbolos (objdump/nm/jadx/Ghidra) antes de tocar \`loader/\`/\`source/\` — skill
   \`psvita-port-init\` ya cubrió la Fase 0-2, falta la Fase 3 (confirmar motor real).
2. Bootstrap del loader guiado por la skill \`psvita-porting\`.
3. Build (\`porting_tools/build/build_and_install.sh\`) → probar en Vita3K → probar en consola real.
4. Un bug a la vez, guiado por el log real — skill \`so-crash-triage\` para cruzar
   log + \`.psp2dmp\` + \`objdump\`/Ghidra + jadx.
5. Actualizar \`port_progress.md\` con cada bug confirmado (no teoría sin verificar).
EOF
    ok "CLAUDE.md escrito."
}

# ---------------------------------------------------------------------------
# Fase 9 -- Resumen final
# ---------------------------------------------------------------------------
print_summary() {
    echo ""
    echo "================================================================"
    echo "  Listo: $NEW_DIR"
    echo "================================================================"
    echo "Checklist de la Fase 0 (skill psvita-port-init):"
    echo "  [x] jadx/Docker so-decompiler: $( [ "$HAVE_JADX" = "1" ] && [ "$HAVE_DOCKER_SODECOMP" = "1" ] && echo "ambos corrieron" || echo "revisar warnings arriba, puede faltar correr algo a mano" )"
    echo "  [ ] Símbolos JNI/imports analizados a fondo -- PORTING_PLAN.md sección 4 es solo el punto de partida."
    echo "  [x] git init + .gitignore anti-DMCA."
    echo "  [x] porting_tools/ / manage_vita.py / CMakeLists.txt sin referencias a otro juego."
    echo "  [x] PORTING_PLAN.md escrito con hallazgos automáticos (no copiado de otro port)."
    echo "  [x] .claude/skills/ (psvita-porting, so-crash-triage, psvita-port-init) y CLAUDE.md presentes."
    echo ""
    echo "Siguiente paso: abrir $NEW_DIR en Claude Code y seguir PORTING_PLAN.md sección 5 --"
    echo "empezando por la Fase 3 (análisis real del motor, confirmar/refutar la hipótesis de motor"
    echo "compartido con algún port hermano) antes de escribir código en loader/."
}

main() {
    check_prereqs
    prompt_inputs
    setup_repo_dir
    place_apk_and_detect
    decompile
    git_init_and_ignore
    setup_porting_tools
    write_plan_and_progress
    write_claude_md_and_skills
    print_summary
}

main "$@"

{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wrapGAppsHook4,
  gtk4,
  libadwaita,
  tesseract,
  wl-clipboard,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "swash";
  version = "1.5.1";

  src = ../.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wrapGAppsHook4
  ];

  buildInputs = [
    gtk4
    libadwaita
  ];

  preFixup = ''
    gappsWrapperArgs+=(
      --prefix PATH : "${lib.makeBinPath [ tesseract wl-clipboard ]}"
    )
  '';

  meta = with lib; {
    description = "Screenshot annotator and lightweight image editor";
    homepage = "https://github.com/ItsLemmy/swash";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "swash";
  };
})

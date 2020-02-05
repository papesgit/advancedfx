echo Requires GNU gettext for Windows to be installed.


echo "Project: hlae"
cd hlae
mkdir ..\deps\release\l10n\locales\en-US\hlae\
xgettext -k -kGetString -kGetPluralString:1,2 -kGetParticularString:1c,2 -kGetParticularPluralString:1c,2,3 -k_ -k_n:1,2 -k_p:1c,2 -k_pn:1c,2,3 -f xgettext-files.txt -L C# --from-code UTF-8 -o ..\deps\release\l10n\locales\en-US\hlae\messages.pot "--copyright-holder=advancedfx.org" "--package-name=hlae" 
cd ..


echo "Project : installer\HlaeCore"
cd installer\HlaeCore
mkdir ..\..\deps\release\l10n\locales\en-US\installer\HlaeCore\
py -3.8 -m pip install polib 
py -3.8 ..\..\deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/en-US.wxl ..\..\deps\release\l10n\locales\en-US\installer\HlaeCore\messages.pot
cd ..\..


echo "Project : installer\setup"
cd installer\setup
mkdir ..\..\deps\release\l10n\locales\en-US\installer\setup\
py -3.8 -m pip install polib 
py -3.8 ..\..\deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/HyperlinkTheme_en-US.wxl ..\..\deps\release\l10n\locales\en-US\installer\setup\messages.pot
cd ..\..

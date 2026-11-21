<a id="japanese"></a>

<div align="center">
  <h2>⚠️ ご利用にあたっての重要な注意</h2>
  <p><strong>本ソフトウェアは、あくまでGrimodexに付随するものです。</strong></p>
  <p>
    通常のIMEとして利用する場合は、ご自身でフォークするか、より純粋な用途を目指した<br>
    フォークの利用をご検討ください。本プロジェクトはマルチプラットフォーム対応のために<br>
    大きく手が入っており、今後Grimodex向けの機能が追加されるほど、信頼境界とそれに伴う<br>
    リスクも大きくなります。
  </p>
</div>

<p align="center">
  <img src="src/data/images/icon.svg" width="112" height="112" alt="Mozkey IbG icon">
</p>

<p align="center"><a href="#english">English</a></p>

<h1 align="center">Mozkey — Integrated by Grimodex</h1>

<p align="center">
  <strong>Mozkey IbG</strong><br>
  koyasi777氏のMozkey（もずきー）を直接のフォーク元とし、その成果を受け継ぎながら、<br>
  Grimodex連携とマルチプラットフォーム対応を加えた日本語入力システムです。
</p>

<p align="center">
  <a href="https://github.com/koyasi777/mozkey"><img alt="Forked from koyasi777/mozkey" src="https://img.shields.io/badge/forked%20from-Mozkey-88A2DD"></a>
  <img alt="Integrated by Grimodex" src="https://img.shields.io/badge/integrated%20by-Grimodex-7857D8">
  <img alt="Project identifier MozkeyIbG" src="https://img.shields.io/badge/id-MozkeyIbG-53D4C7">
  <img alt="Repository and package name mozkey-ibg" src="https://img.shields.io/badge/repository%20%2F%20package-mozkey--ibg-D99A43">
</p>

Mozkey — Integrated by Grimodexは、
[koyasi777氏が開発・公開するMozkey（もずきー）](https://github.com/koyasi777/mozkey)
から派生したフォークです。
フォーク元Mozkeyで築かれたライブ変換、ローカルZenz補正、文脈補正、入力・UIの
カスタマイズを継承し、Grimodexのプロジェクト辞書連携、対応プラットフォーム、
配布・検証基盤などを拡張しています。本文では主に**Mozkey IbG**と表記します。

本プロジェクトはフォーク元Mozkeyの公式配布物ではなく、フォーク元によるサポートや
品質保証の対象ではありません。また、Google日本語入力でも、Googleまたはgoogle/mozcの
公式配布物でもありません。

## フォークの系譜と謝辞

このリポジトリの直接のフォーク元は**MozcではなくMozkey**です。

[`google/mozc`](https://github.com/google/mozc) →
[`koyasi777/mozkey`](https://github.com/koyasi777/mozkey) →
`Mozkey — Integrated by Grimodex`

Mozkey IbGの土台は、koyasi777氏とMozkeyのコントリビューターが積み重ねてきた実装、
設計、検証、ドキュメントです。その成果がなければ本プロジェクトは成立しません。
ここに敬意と感謝を表します。正式名の先頭に`Mozkey`を残しているのも、フォーク元の
プロジェクト名と成果を明示的に受け継ぐためです。
`Integrated by Grimodex`は、この派生版で行う統合と保守を示すcreditであり、
Mozkeyの由来やフォーク元のcreditを置き換えるものではありません。

Mozkey自体はgoogle/mozcを基盤としているため、Mozcとそのコントリビューターへの
著作権表示、ライセンス、attributionも引き続き保持します。

現在のMozkey IbGは、フォーク元の**Mozkey v1.0.0**に追従しています。Mozkey v1.0.0までの
変換・学習・UIの機能、修正、既定値を取り込んだうえで、Mozkey IbG固有の製品識別子、
Grimodex連携、Linux / Fcitx5対応、セキュリティ強化を重ねています。Mozkey IbG自体の
release versionはフォーク元とは独立しており、`src/version.bzl`で管理します。

## 名称と識別子

| 用途 | 表記 |
| --- | --- |
| 正式名 | **Mozkey — Integrated by Grimodex** |
| 省略表記 | **Mozkey IbG** |
| 論理識別子 | `MozkeyIbG` |
| リポジトリ／パッケージ名 | `mozkey-ibg` |
| 読み | Mozkey（もずきー） |

`MozkeyIbG`は本プロジェクトを示す論理識別子、`mozkey-ibg`はリポジトリと
配布パッケージに使用する標準名です。ただし、`mozkey-ibg`という名前のパッケージが
OSのパッケージリポジトリですでに公開されている、という意味ではありません。

インストール・登録・実行時にOSから参照される製品識別子は、`mozkey-ibg` /
`MozkeyIbG`名前空間へ分離しています。このため、フォーク元Mozkeyとは別製品として
同時にインストールできます。上流追従のため、専用ディレクトリ内の実行ファイル名や
ソースコード上の名前には`mozc`が残りますが、共有登録や共有IPCには使用しません。
プラットフォーム別の識別子と非移行方針は
[Mozkey IbG product identity](docs/mozkey_ibg_product_identity.md)を参照してください。

GitHubリポジトリは
[`kazormia296/mozkey-ibg`](https://github.com/kazormia296/mozkey-ibg)です。

## 対応状況

| プラットフォーム | 配布・検証対象 | 現在の位置づけ |
| --- | --- | --- |
| Windows | x64 / universal / ARM64 MSI | 主な利用対象。未署名のexperimental build |
| macOS | Apple Silicon向けarm64 pkg | native build・実推論probe・Developer ID署名・notarization必須 |
| Debian / Ubuntu | amd64 deb | Ubuntu 24.04 native build・同梱runtime・multiarch layout検証 |
| Fedora / RPM | x86_64 rpm | Fedora 44 native build・同梱runtime・`/usr/lib64` layout検証 |
| Arch Linux | x86_64 AUR / Fcitx5 payload | 初回公開Releaseから`mozkey-ibg-bin`を登録・更新するautomationを実装済み |
| Android / iOS | — | Grimodex本体の対象外。製品build・CI・releaseなし |

Android / iOS由来のコードは上流Mozkey / Mozcへの追従性を保つため残っていますが、
Mozkey IbGの製品build・検証・配布ターゲットではありません。

> [!WARNING]
> tag release workflowが生成する配布物はexperimental / pre-releaseです。Windows MSIは
> 未署名です。macOS pkgは署名・notarize検証を通過しますが、公開後にrelease notes、
> checksum、既知の制限も確認してください。

## ダウンロードとインストール

初回公開後の成果物は[GitHub Releases](https://github.com/kazormia296/mozkey-ibg/releases)
から取得できます。製品ビルドは`src/version.bzl`と一致する`vX.Y.Z`タグからのみ実行し、
確認用のdraft prereleaseを経て公開します。

主な成果物名は次のとおりです。

- `MozkeyIbG_vX.Y.Z_x64.msi`
- `MozkeyIbG_vX.Y.Z_universal.msi`
- `MozkeyIbG_vX.Y.Z_arm64.msi`
- `MozkeyIbG_vX.Y.Z_macos_arm64.pkg`
- `mozkey-ibg_X.Y.Z_amd64.deb`
- `mozkey-ibg-X.Y.Z-1.x86_64.rpm`
- `mozkey-ibg-vX.Y.Z-archlinux-x86_64.tar.xz`
- `SHA256SUMS`

成果物名も`MozkeyIbG` / `mozkey-ibg`に統一しています。

### Windows

通常の64-bit Windowsでは`MozkeyIbG_vX.Y.Z_x64.msi`を使用してください。ARM64環境では
native ARM64版が通常の選択です。`universal`版はARM64 / ARM64ECのTIP bridgeを追加しつつ、
server、Zenz helper、runtimeをx64のままWindows-on-ARM emulationで動かす互換構成です。
インストール前後の詳しい手順とbuild要件は
[Windows build guide](docs/build_mozc_in_windows.md)を参照してください。

### macOS

現在のpkgはApple Silicon向けarm64 buildです。macOS 12以降を対象にnative CIで
package layoutとローカルZenz runtimeの実推論を検証し、release時にはnested codeと
installerをDeveloper ID署名してApple notarizationとGatekeeper検証を行います。
署名・notarizationに必要なsecretがないreleaseはfail closedします。詳細は
[macOS build guide](docs/build_mozc_in_osx.md)を参照してください。

### Linux / Fcitx5

Linux releaseはDebian / Ubuntu向けamd64 `.deb`、Fedora / RPM系向けx86_64 `.rpm`、
Arch Linux向けAUR packageと検証済みpayloadを生成します。IMEはFcitx5のsystem addonを
登録するため、単体application向けのAppImageは生成しません。

`.deb`と`.rpm`は、release CIが対象ディストリ上でnative buildしたMozkeyと、固定した
CPU-only `llama-server`を同梱します。公開Releaseからダウンロード後、次のように
package managerで導入できます。

```sh
sudo apt install ./mozkey-ibg_X.Y.Z_amd64.deb
sudo dnf install ./mozkey-ibg-X.Y.Z-1.x86_64.rpm
```

Arch Linuxでは、最初の公開Release後にAURへ登録される`mozkey-ibg-bin`を使用します。
AUR packageは公開ReleaseのArch payloadとchecksumを検証して導入し、実行時の
`llama-server`はArchの`llama-cpp` packageを使用します。AUR登録前または開発時は、
次のArch Linux x86_64専用のsource install経路を利用できます。

fresh checkoutからrelease構成をbuild・検証・installする基本経路は次のとおりです。

```sh
scripts/build_mozkey_linux_bazel archlinux-x86_64
cd src
../scripts/preflight_mozkey_linux_bazel
../scripts/smoke_test_mozkey_fcitx5_install
sudo env PREFIX=/usr ../scripts/install_mozkey_linux_bazel
```

対応するsystem path、外部依存、staging smoke、checksum、SPDX、rollback、uninstallは
[Linux product isolation](docs/linux_product_isolation.md)にまとめています。

## 主な特徴

### ライブ変換と入力補助

- 入力後の短いdelayを調整できるライブ変換
- 変換前の読みを表示するruby風overlay（Windows / macOS）／候補欄上部表示（Linux Fcitx5）
- 最小文字数、装飾記号、入力途中の口語表現を考慮した過剰変換の抑制
- ライブ変換を使わない場合の、初回変換操作での候補window表示
- 曖昧なローマ字規則でも入力途中を表示できるオプション
- カスタムローマ字テーブルの1規則あたりの出力をUTF-8で1024 bytes未満まで拡張
  （入力・次入力は従来どおり300 bytes未満）
- 「郵便番号変換」有効時に、`1000001`のようなハイフンなし7桁からも住所候補を取得。
  ライブ変換ONでは数字表記を第1候補に保ち、住所は後続候補として残す
- 日付・曜日の変換候補と、日常語彙・固有名詞・口語表現の補強

### 文脈を使った変換

- 確定済みの左文脈と限定的な右文脈を使った候補順位の補正
- 助詞、複合機能語、名詞述語、条件表現、地名・施設名構成の誤分割抑制
- ユーザー辞書由来表記、ASCII、mixed-script、記号styleの保護
- secure inputではsurrounding textを読まない境界設計

### ローカルZenz補正

- llama.cpp / `llama-server`を使うローカル推論
- 外部AI serviceに入力を送らず、platformごとに分離したlocal helperで補正
- delay、最小文字数、device、profile / topic / styleなどを設定可能
- 通常のライブ変換結果を先に表示する方式に加え、Zenzの応答を一定時間待ってから
  表示する方式も選択可能
- Zenz候補が通常Mozc結果の明白な先頭・末尾の欠落になっている場合は拒否してMozc結果へ
  戻す保護と、採用前にMozcで逆変換して読みを照合する「厳密な読み保持（実験的）」
- 「Zenz学習（Mozc変換履歴）」と「Zenz学習（補正フィードバック）」を独立して設定
- 短い読みではZenz feedbackを自動適用しない制限（既定ON、最小読み文字数7文字）。
  記録は継続し、明示的な手動blockは読みの長さにかかわらず有効
- accepted / rejected feedback、明示block、検索、import / export、削除UI
- 通常Mozc候補を削除せず、local feedbackを保守的に順位へ反映

### 入力・学習・keymap

- 句読点・記号を入力した時点で未確定文字列を確定するオプション
- 変換cancel後にそのまま確定したひらがな表記の学習
- 確定直後のBackspace、Cancel、Undoを考慮した履歴学習の取り消し
- 入力途中のサジェストを明示的に選んで確定した場合の、全文の読みと文節単位の学習
- 1つのkeyへ`Commit → IMEOff`のような複数commandを順序付きで割り当て
- 待機stepを挟める遅延command列と、手動Zenz補正command
  （`ForceZenzLiveCorrection`、feedbackを無視する
  `ForceZenzLiveCorrectionWithoutFeedback`）
- 「ライブ変換中」（`LiveConversion`）と「Zenz補正中」（`ZenzConversion`）専用のkeymap状態
- 選択中の候補を履歴から削除、または候補一覧から非表示にする`DeleteSelectedCandidate`
  （標準keymapではPrediction / Conversionの`Ctrl+Delete`）
- 顔文字・絵文字候補が選択履歴によって第1候補へ上がらないようにする抑制
  （候補自体は第2候補以下に残る）
- Windowsで選択文字列をSpace再変換するMS-IME風設定
- 左右Shift / Ctrlの個別割り当て（Windows）
- Shiftなどによる一時英数字入力から、`IMEOn`で未確定文字列を保ったままひらがなへ復帰
  （Windows）
- 変換中に別の文節区切りへ順に切り替える`CycleSegmentation` shortcut
  （Mozkey IbG固有。標準では`Ctrl+Shift+Space`に割り当て済みで、設定画面の
  Mozkey IbG tabから無効化可能）

### Windows / macOS UI

- 候補、suggest、ruby表示のlight / dark / 自動（system themeに追従）/ custom theme。
  新規設定では3項目とも「自動」が既定（Windows / macOS）
- 色、size、corner radius、opacity、shadowの調整（Windows / macOS）。fontと文字の太さは
  Windowsで調整可能
- 未確定文字の近くに「入力中・Mozc・Zenz処理中・Zenz適用済み」を表示する
  変換状態indicator（Windows / macOS）。配色・形状・位置を調整可能。macOSの新しい
  indicatorは実機検証が未完了
- 縦書き対応（Windows / macOS）: 候補、suggest、用例、rubyを縦組みに合わせて配置し、
  対応するkeymapでは矢印keyを視覚方向に合わせて解釈。横書きの既存動作は維持
- 未確定文字の文字色、背景色、下線色の設定と、変換規則の途中にある未確定ローマ字を
  薄く表示するopt-in設定（Windows）
- IME mode indicatorのlight / dark theme追従（Windows）
- 既定IMEの設定と以前の設定へのrestore（Windows）
- 既定・monochrome black・monochrome whiteのIME icon（Windows）
- Windows Searchなどのimmersive TSF環境で、候補、suggest、rubyがhostの背面へ
  隠れないようにする表示経路
- TSF input scopeの`IS_PRIVATE`（Chrome Incognito、Edge InPrivateなど）を
  private inputとして扱い、既存履歴の読み取りは維持したままMozc履歴とZenz feedbackへの
  新規書き込みを停止。入力mode iconとindicatorに盾を重ねて表示

各機能の詳細な仕様は、フォーク元
[Mozkey v1.0.0のREADME](https://github.com/koyasi777/mozkey/blob/v1.0.0/README.md)
にまとまっています。ただし、そこに書かれたdownload・install手順と製品識別子は
フォーク元Mozkeyのものであり、Mozkey IbGには適用されません。

### Grimodex連携

- Grimodexのproject dictionaryをversion単位で安全に読み込み
- applicationごとに対象projectを分離し、更新途中のdictionaryを参照しない設計
- password欄などのsecure inputではsurrounding textを参照しない
- install / uninstall後に古いconsumer情報を残しにくいplatform別lifecycle
- Linux Fcitx5でもMozkey IbG専用のproduct dataとして分離

Grimodexが起動していない場合も、Mozkey IbGの通常変換は利用できます。Grimodex連携は、
project dictionaryと安全なapplication contextを追加するためのoptional integrationです。
詳細は[Grimodex desktop integration](docs/grimodex_desktop_integration.md)を参照してください。

## プライバシーとネットワーク

Mozkey IbGは、通常のIME利用時に入力内容を外部Internet serviceへ送信しない構成を
目指しています。

- 使用統計とcrash reportのUIを削除し、通常経路のstats実装をNullに固定
- Zenz推論はlocal modelとlocalhost endpointだけを使用
- Zenzに渡す文脈からURL、email、path、token、長い数字列などを除外
- feedbackにはraw left contextを保存せず、full sequenceと非可逆なcontext classだけを保存
- Windows向けにnetwork DLL import / string markerの検査toolと、Firewall ruleを確認する
  release checklistを提供
- secure inputではsurrounding textを送らない
- Secret / Incognito modeではZenz feedbackの参照と記録を停止し、WindowsのTSF
  `IS_PRIVATE`では既存の学習を読み取り専用として新規の永続的な学習書き込みを停止

localhost内部通信は、外部Internet通信とは別のローカルprocess境界として使用します。
また、dependency、dictionary source、toolchainの取得を伴うbuild・test・release処理では
network accessが必要になる場合があります。正確な保証範囲と検証項目は
[Secure Offline Guarantee](docs/security/offline_guarantee.md)および
[Release Checklist](docs/security/release_checklist.md)を参照してください。

## 辞書profile

| profile | 用途 | 外部語彙の扱い |
| --- | --- | --- |
| `release-approved-only` | 公開release | source、commit、SHA-256、licenseを固定した再配布可能な入力だけを使用 |
| `local-evaluation`（`daily`生成辞書） | ローカル評価 | Nico/Pixivなどを含められるが、公開成果物からは除外 |

大きな生成辞書はrepositoryへcommitせず、
`src/data/dictionary_koyasi/generated/`以下へローカル生成します。公開releaseでは
Windows、macOS、Linuxのすべてで`release-approved-only`を使用します。

Windows / macOS向けのローカル生成例:

```powershell
.\tools\dictionary\prepare_daily_dictionary.ps1
```

source、license、quality gate、Linux-native生成については
[Dictionary data and quality policy](src/data/dictionary_koyasi/README.md)と
[Third-party notices](THIRD_PARTY_NOTICES.md)を参照してください。

## Build・CI・release

通常のpull requestと`main` pushでは、辞書準備、test、lint、offline checkを実行し、
installerや配布payloadは生成しません。製品buildはrelease workflowから
`release: true`で呼び出された場合だけ実行されます。

releaseは次の条件をすべて満たす必要があります。

1. tagが正規形`vMAJOR.MINOR.PATCH`である
2. tagと`src/version.bzl`のversionが一致する
3. tagged commitが`origin/main`に含まれる
4. platform testとproduct verificationが成功する

成功後はWindows、macOS、Linuxの成果物と`SHA256SUMS`を1つのdraft
prereleaseへまとめます。公開済みreleaseをworkflowから上書きすることはありません。
手順は[Releasing Mozkey](docs/releasing.md)を参照してください。

platform別のbuild資料:

- [Windows](docs/build_mozc_in_windows.md)
- [macOS](docs/build_mozc_in_osx.md)
- [Linux](docs/build_mozc_for_linux.md)
- [Docker build](docs/build_mozc_in_docker.md)

## ドキュメント

- [Configuration](docs/configurations.md)
- [Linux product isolation](docs/linux_product_isolation.md)
- [Linux dictionary quality gate](docs/linux_daily_dictionary_quality_gate.md)
- [Grimodex desktop integration](docs/grimodex_desktop_integration.md)
- [Secure Offline Guarantee](docs/security/offline_guarantee.md)
- [Secure Offline Release Checklist](docs/security/release_checklist.md)
- [Release procedure](docs/releasing.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)
- [Contributing](CONTRIBUTING.md)

## License・フォーク元・upstream attribution

Mozkey IbGの直接のフォーク元は[koyasi777/mozkey](https://github.com/koyasi777/mozkey)です。
本リポジトリには、koyasi777氏とMozkeyコントリビューターによるcode、設計、文書、
改善が含まれています。変更履歴を含め、フォーク元の著作と貢献を尊重します。
フォーク元が使用しているcredit
`Copyright 2026 koyasi777. Portions Copyright 2010-2026 Google LLC.`も保持します。

MozkeyはMozcを基盤としているため、本リポジトリにもMozc由来のcodeが含まれます。
Googleが作成したMozc codeは[BSD 3-Clause License](LICENSE)で提供され、第三者code・
dictionary・model・runtimeにはそれぞれのlicenseが適用されます。配布・改変時は
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)と`LICENSES/`も確認してください。

- 直接のフォーク元: [koyasi777/mozkey](https://github.com/koyasi777/mozkey)
- さらに上流のプロジェクト: [google/mozc](https://github.com/google/mozc)

---

<a id="english"></a>

# English

[日本語](#japanese)

## Mozkey — Integrated by Grimodex

**Mozkey IbG** is a downstream fork of
[Mozkey by koyasi777](https://github.com/koyasi777/mozkey). It inherits Mozkey's
live conversion, local Zenz correction, context-aware ranking, and configurable
input and UI behavior, then extends them with Grimodex project dictionaries,
additional platform support, and release verification.

It is not an official upstream Mozkey release and is not supported or warranted
by the upstream project. It is also not Google Japanese Input or an official
Google / google/mozc distribution.

### Lineage and acknowledgements

The direct upstream of this repository is **Mozkey, not Mozc**:

[`google/mozc`](https://github.com/google/mozc) →
[`koyasi777/mozkey`](https://github.com/koyasi777/mozkey) →
`Mozkey — Integrated by Grimodex`

This project exists because of the implementation, design, testing, and
documentation built by koyasi777 and the Mozkey contributors. We gratefully
acknowledge their work and retain `Mozkey` at the front of the formal name to
make that heritage explicit. Mozkey is itself based on google/mozc, so the
copyright, license, and attribution for Mozc and its contributors are preserved
as part of the full lineage.

`Integrated by Grimodex` credits the integration and maintenance performed in
this downstream fork; it does not replace Mozkey's origin or upstream credit.

Mozkey IbG currently tracks upstream **Mozkey v1.0.0**. It adopts the
conversion, learning, and UI features, fixes, and defaults up to that release,
and layers the Mozkey IbG product identity, Grimodex integration, Linux / Fcitx5
support, and security hardening on top. Mozkey IbG keeps its own release version,
independent of upstream, in `src/version.bzl`.

### Project identity

| Purpose | Name |
| --- | --- |
| Formal name | **Mozkey — Integrated by Grimodex** |
| Short name | **Mozkey IbG** |
| Logical identifier | `MozkeyIbG` |
| Repository / package name | `mozkey-ibg` |

`MozkeyIbG` is the logical project identifier, and `mozkey-ibg` is the canonical
repository/package slug. It does not imply that a package-manager package is
already available. Product registration, installation, profile, and IPC
identifiers use the `MozkeyIbG` / `mozkey-ibg` namespace so the upstream Mozkey
product can be installed at the same time. Source-level names and executable
basenames may still contain `mozc` when they live below product-private paths.
See [Mozkey IbG product identity](docs/mozkey_ibg_product_identity.md) for the
platform contract and the intentional no-migration policy.

The current GitHub URL is
[`kazormia296/mozkey-ibg`](https://github.com/kazormia296/mozkey-ibg).

### Platform status

| Platform | Output | Status |
| --- | --- | --- |
| Windows | x64 / universal / ARM64 MSI | Primary target; unsigned experimental builds |
| macOS | Apple Silicon arm64 pkg | Native build and inference probe; Developer ID signing and notarization required |
| Debian / Ubuntu | amd64 deb | Ubuntu 24.04 native build, bundled runtime, and multiarch validation |
| Fedora / RPM | x86_64 rpm | Fedora 44 native build, bundled runtime, and `/usr/lib64` validation |
| Arch Linux | x86_64 AUR / Fcitx5 payload | Automation registers and updates `mozkey-ibg-bin` from the first public release |
| Android / iOS | — | Outside Grimodex scope; no product build, CI, or release |

Inherited Android and iOS code remains to ease upstream Mozkey / Mozc maintenance,
but it is not a Mozkey IbG product build, verification, or distribution target.

Product builds run only for a validated `vX.Y.Z` tag and are collected in a
reviewed draft prerelease. After the first publication, artifacts will be
available from [GitHub Releases](https://github.com/kazormia296/mozkey-ibg/releases).

### Downloads and installation

Expected release assets include:

- `MozkeyIbG_vX.Y.Z_x64.msi`, `MozkeyIbG_vX.Y.Z_universal.msi`, and
  `MozkeyIbG_vX.Y.Z_arm64.msi`
- `MozkeyIbG_vX.Y.Z_macos_arm64.pkg`
- `mozkey-ibg_X.Y.Z_amd64.deb`
- `mozkey-ibg-X.Y.Z-1.x86_64.rpm`
- `mozkey-ibg-vX.Y.Z-archlinux-x86_64.tar.xz`
- `SHA256SUMS`

Use `x64` on ordinary 64-bit Windows. On Windows ARM64, the native `arm64`
package is the normal choice; `universal` keeps the server and local Zenz
runtime on x64 emulation while adding ARM64 / ARM64EC TIP bridges. The macOS
package targets Apple Silicon on macOS 12 or later. Release packaging fails
closed unless it is signed with Developer ID identities, notarized, stapled,
and assessed by Gatekeeper.

Linux releases provide an amd64 Debian/Ubuntu package, an x86_64 Fedora/RPM
package, and the Arch `mozkey-ibg-bin` AUR package backed by the verified Arch
payload. AppImage is intentionally not provided because the Fcitx5 input method
must install system addon and input-method metadata. The deb and RPM bundle the
pinned CPU-only local inference runtime; Arch uses the distribution
`llama-cpp` runtime. See [Linux product isolation](docs/linux_product_isolation.md)
for exact dependencies, attestation, rollback, and uninstall behavior.

### Highlights

- Debounced live conversion with a Windows / macOS ruby overlay or Fcitx5
  reading display
- Local Zenz inference without an external AI service, shown either after the
  immediate Mozc live conversion or after briefly waiting for Zenz
- Obvious-omission guards that fall back to the Mozc result, plus an
  experimental strict reading-preservation check before a Zenz result is adopted
- Context-aware ranking while preserving user-dictionary and mixed-script forms
- Conservative local feedback learning and explicit feedback management, with
  independent switches for Mozc-history learning and Zenz feedback, and no
  automatic feedback reuse for short readings (on by default, minimum 7
  characters)
- Learning of complete readings and segment preferences from explicitly accepted
  suggestions
- Direct punctuation commit, multi-command key bindings with delay steps, manual
  Zenz correction commands, dedicated `LiveConversion` / `ZenzConversion` keymap
  states, and correction-aware history
- Deleting a selected candidate from history or hiding it
  (`DeleteSelectedCandidate`, `Ctrl+Delete` in the default keymaps), and keeping
  emoticon and emoji candidates out of the top position
- Hyphenless seven-digit postal-code conversion, custom romaji-table outputs up
  to 1023 UTF-8 bytes per rule, and extended date and weekday candidates
- Configurable candidate, suggestion, and ruby appearance on Windows and macOS
  (light / dark / system / custom; new profiles default to system), plus
  Windows font, preedit, pending-romaji dimming, icon, and mode UI
- A conversion-status indicator on Windows and macOS (Reading, Mozc, Zenz
  processing, Zenz applied); the new macOS indicator has not yet been verified
  on native hardware
- Vertical-writing layouts for candidate, suggestion, infolist, and ruby windows
  on Windows and macOS, with keymap navigation that follows the visual direction
- Windows Search / immersive TSF host support, TSF `IS_PRIVATE` private-input
  handling that stops new history and feedback writes, and returning to hiragana
  from temporary ASCII input with `IMEOn`
- A `CycleSegmentation` shortcut that steps through alternative segmentations
  during conversion (Mozkey IbG only; bound to `Ctrl+Shift+Space` by default and
  switchable on the Mozkey IbG tab of the configuration tool)
- Safe Grimodex project dictionaries with secure-input boundaries
- Pinned public dictionary profile separated from local-evaluation data

The detailed behavior of the inherited features is documented in the upstream
[Mozkey v1.0.0 README](https://github.com/koyasi777/mozkey/blob/v1.0.0/README.md).
Its download and installation instructions and product identifiers describe
upstream Mozkey and do not apply to Mozkey IbG.

Mozkey IbG aims to keep normal IME input off external Internet services. Local
Zenz uses a loopback endpoint as an internal process boundary; build and release
tasks may still access the network to fetch pinned dependencies. See the
[offline guarantee](docs/security/offline_guarantee.md) for the exact boundary.

### Documentation

- [Downloads and releases](https://github.com/kazormia296/mozkey-ibg/releases)
- [Release procedure](docs/releasing.md)
- [Windows build](docs/build_mozc_in_windows.md)
- [macOS build](docs/build_mozc_in_osx.md)
- [Linux product isolation](docs/linux_product_isolation.md)
- [Grimodex integration](docs/grimodex_desktop_integration.md)
- [Dictionary policy](src/data/dictionary_koyasi/README.md)
- [Security and privacy](docs/security/offline_guarantee.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)
- [Fork source: koyasi777/mozkey](https://github.com/koyasi777/mozkey)

Mozkey IbG is directly forked from
[koyasi777/mozkey](https://github.com/koyasi777/mozkey) and includes work by
koyasi777 and the Mozkey contributors. Through that lineage, it also contains
code derived from Mozc under the [BSD 3-Clause License](LICENSE). Third-party
components retain their respective licenses and attribution requirements.
The upstream credit is preserved as
`Copyright 2026 koyasi777. Portions Copyright 2010-2026 Google LLC.`

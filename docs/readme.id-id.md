[简体中文](../README.md) | [English](readme.en-us.md) | [Русский](readme.ru-ru.md) | Bahasa Indonesia

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/brand/huhucapture-logo-dark.svg">
  <img src="../assets/brand/huhucapture-logo-light.svg" alt="huhucapture" width="560">
</picture>

# huhucapture

**huhucapture** adalah aplikasi Windows native untuk tangkapan layar, anotasi, dan perekaman layar yang dipelihara oleh [NAMEWTA](https://github.com/NAMEWTA). Aplikasi ini menyatukan area bernomor dan penjelasannya dalam satu gambar, serta mendukung gambar mengambang, tangkapan panjang, rekaman GIF / MP4, dan pembacaan kode QR.

Proyek ini merupakan bagian dari keluarga merek [huhu](https://github.com/NAMEWTA/huhu). Identitasnya memakai karakter Huhu dan sudut bingkai tangkapan. Aset serta aturan penggunaannya tersedia di [panduan merek](brand.md).

[Unduh v0.0.2](https://github.com/NAMEWTA/huhucapture/releases/tag/v0.0.2) · [Semua rilis](https://github.com/NAMEWTA/huhucapture/releases) · [Laporkan masalah](https://github.com/NAMEWTA/huhucapture/issues)

## Area dan penjelasan dalam satu gambar

![Contoh gambar gabungan dengan area bernomor dan catatan](banner.png)

Contoh ini disalin langsung dari editor aplikasi. Bingkai area, nomor kecil di sudut kiri atas, dan catatan terkait memakai warna otomatis yang sama. Menghapus satu anotasi tidak mengubah nomor atau warna anotasi lain, sehingga nomor dapat memiliki jeda.

1. Pilih area tangkapan lalu buka editor gambar, atau tahan `Ctrl` ketika menyelesaikan pemilihan area.
2. Pilih **Anotasi** di samping alat **Nomor** biasa.
3. Tahan tombol kiri mouse dan tarik persegi panjang ke arah mana pun. Setelah dilepas, tulis catatan multibaris di panel kanan.
4. Klik nomor, bingkai, atau baris daftar untuk mengedit catatan. Tarik nomor atau bingkai untuk memindahkan area; gunakan titik kendali untuk mengubah ukurannya.
5. Simpan atau salin untuk menghasilkan satu gambar berisi gambar asli, semua tanda, area bernomor, dan catatan lengkap.

Tarikan yang terlalu kecil tidak menghasilkan anotasi. Bagian dalam area tetap transparan dan dapat memuat tanda atau anotasi bertingkat. Menutup panel tidak menghapus catatan dari hasil ekspor. Nomor dan warna tetap sama ketika membatalkan, mengulang, atau menghapus anotasi lain.

Ekspor membungkus teks serta menyesuaikan ukuran huruf dan jarak, lalu menambah tinggi kanvas atau kolom catatan bila diperlukan. Piksel gambar asli tetap dipertahankan. Jika hasil lengkap melampaui batas perangkat atau memori, aplikasi menampilkan kegagalan dan mempertahankan sesi penyuntingan; catatan tidak dipotong diam-diam. Lihat [panduan anotasi](annotations.md).

## Fitur

- Tangkapan area layar, pemilih warna, gambar mengambang dengan zoom, dan tangkapan panjang dengan menggulir.
- Rekaman GIF / MP4; MP4 menyediakan pilihan audio sistem dan mikrofon.
- Persegi panjang, elips, panah, nomor biasa, garis, teks, mosaik, dan penghapus.
- Anotasi persegi panjang dengan warna otomatis, nomor tetap, dan editor catatan bersama.
- Membaca kode QR dan menyalin isinya.
- Ikon tray, mode sekali pakai, pengaturan portabel, dan beberapa bahasa.

Toolbar memakai **area klik 44 × 44 piksel logis dan ikon 24 piksel logis**, mengikuti DPI dan membagi tombol ke baris berikutnya ketika ruang terbatas. OCR tidak tersedia.

## Riwayat tangkapan dan rekaman

Menyimpan atau menyelesaikan penyalinan otomatis mengarsipkan tangkapan, gambar beranotasi, tangkapan panjang, GIF, dan MP4 ke folder `huhucapture` di dalam Documents Windows. Lokasi mengikuti pengalihan folder sistem. Nama memakai waktu lokal `YYYYMMDD-HHMMSS.ext`; benturan pada detik yang sama mendapat akhiran `-001`, `-002` tanpa menimpa berkas sebelumnya.

Buka riwayat melalui ikon tepat di sebelah kanan pembacaan QR pada toolbar tangkapan. Daftar menampilkan gambar mini dan sampul video, dengan tindakan untuk menyalin atau membuka foldernya. Riwayat tetap tersedia setelah aplikasi dibuka kembali. Membatalkan Save As tetap menyimpan rekaman yang selesai; menyimpan ke folder lain tetap meninggalkan salinan arsip.

Riwayat juga dapat dibuka langsung dari menu tray. Hapus semua meminta konfirmasi dan secara default memindahkan media yang tercantum ke Recycle Bin. Jika Windows tidak dapat mendaur ulangnya, Windows meminta konfirmasi tambahan sebelum penghapusan permanen. Berkas lain, subfolder, dan salinan di lokasi lain tidak diproses. Kegagalan atau pembatalan menyegarkan daftar berkas yang tersisa.

Pengaturan ditata ulang mengikuti [desain Apple](https://github.com/VoltAgent/awesome-design-md/blob/main/design-md/apple/DESIGN.md) yang diminta: latar netral, kelompok putih, teks lebih besar, kontrol konsisten, dan satu warna biru untuk interaksi.

## Unduh dan jalankan

Memerlukan **Windows 10 versi 1803 atau lebih baru, x64**. Unduh `huhucapture.exe` dari [Releases](https://github.com/NAMEWTA/huhucapture/releases), lalu jalankan langsung. Tidak perlu pemasangan aplikasi atau runtime Visual C++ secara terpisah.

Untuk pemakaian biasa, pilih EXE atau ZIP portabel.

| Berkas | Kegunaan |
| --- | --- |
| `huhucapture.exe` | Program x64 siap jalan dengan bahasa Mandarin Sederhana dan Inggris |
| `huhucapture-v0.0.2-windows-x64.zip` | EXE yang sama, bahasa tambahan, lisensi, dan informasi build |
| `huhucapture-v0.0.2-windows-x64-symbols.zip` | Simbol PDB untuk debugging dan analisis crash; tidak diperlukan untuk menjalankan aplikasi |
| `build-info.json` | Commit sumber, versi, arsitektur, toolchain, dependensi, dan SHA256 EXE |
| `SHA256SUMS.txt` | Nilai SHA256 untuk empat aset rilis lainnya |
| `Source code (zip / tar.gz)` dari GitHub | Snapshot sumber pada tag, tanpa EXE hasil build |

Untuk debugging, ekstrak PDB dari **build yang sama** dengan EXE lalu tambahkan foldernya ke jalur pencarian simbol Visual Studio atau WinDbg. PDB dari build lain tidak dapat dianggap cocok hanya karena nomor versinya sama. Gunakan commit dalam `build-info.json` untuk membaca sumber. Jalankan `Get-FileHash .\huhucapture.exe -Algorithm SHA256` dan cocokkan hasilnya dengan `SHA256SUMS.txt`.

Keluar dari aplikasi sebelum mengganti berkas secara manual. Pembaruan bawaan meminta persetujuan. Pintasan tangkapan default kini **F1**: nilai default lama `Ctrl+Alt+A` dipindahkan ke F1, sementara pintasan khusus lainnya dan konfigurasi yang sudah ada tetap dipakai.

| Tindakan | Pintasan |
| --- | --- |
| Mulai tangkapan (pintasan global default) | `F1` |
| Simpan seluruh gambar | `Ctrl+S` |
| Salin seluruh gambar | `Ctrl+C`, `Enter`, atau klik ganda saat tidak mengedit teks |
| Batalkan / ulangi gambar | `Ctrl+Z` / `Ctrl+Y` saat tidak mengedit teks |
| Batasi bentuk biasa | Tahan `Shift` saat menggambar |
| Zoom gambar mengambang | `Ctrl` + roda mouse |
| Salin RGB / HEX / CMYK | `Ctrl+R` / `Ctrl+H` / `Ctrl+K` saat memilih warna |

Ketika kolom teks aktif, pengetikan, pemilihan, penghapusan, dan `Ctrl+C` ditangani kolom tersebut. `Ctrl+S` tetap menyimpan gambar lengkap. Tekan `Esc` untuk mengakhiri penyuntingan teks sebelum menyalin gambar. Editor anotasi bersama belum memiliki riwayat pembatalan teks tersendiri; `Ctrl+Z` / `Ctrl+Y` saat mengetik tidak menghapus seluruh anotasi.

## Baris perintah dan pengaturan

```powershell
huhucapture.exe --auto-quit=true
huhucapture.exe --enter=pin
huhucapture.exe --enter=long
huhucapture.exe --enter=video
huhucapture.exe --enter=qr
huhucapture.exe --enter=tray
huhucapture.exe --enter=long --auto-quit=true
```

`--enter=qrcode` adalah alias untuk `--enter=qr`. Opsi ini memilih alur kerja desktop interaktif.

Pengaturan default berada di `%appdata%\huhucapture\config.json`. Jika `config.json` sudah ada di samping EXE, berkas tersebut diprioritaskan; buat berkas kosong untuk mengaktifkan pengaturan portabel. Bahasa tambahan dimuat dari folder `Lang` di samping EXE. Jika folder itu tidak ada, aplikasi memakai `%appdata%\huhucapture\Lang`; kedua lokasi tidak digabungkan. Bahasa Mandarin Sederhana dan Inggris sudah tertanam. Lihat [berkas bahasa](languages.md).

## Pengembangan

Gunakan Windows x64, Visual Studio 2026 dengan komponen pengembangan desktop C++, MSVC v145, ATL dan Windows SDK, PowerShell 7, Git, serta Rustup.

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

Hasil program berada di `artifacts/bin/x64/Release/huhucapture.exe`; solusi proyek adalah `huhucapture.slnx`. Panduan teknis lengkap saat ini berbahasa Mandarin: [build dan rilis](building.md), [arsitektur](architecture.md), [validasi](validation.md), serta [kontribusi](../CONTRIBUTING.md). Pengujian otomatis tidak menggantikan pemeriksaan input fisik, IME, DPI campuran, atau audio.

## Lisensi

Lihat [lisensi MIT](../LICENSE) dan [pemberitahuan pihak ketiga](../third_party/NOTICE.md). Pertahankan lisensi serta pemberitahuan hak cipta yang berlaku saat mendistribusikan aplikasi.

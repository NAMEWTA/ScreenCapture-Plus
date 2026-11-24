[简体中文](../README.md) | [English](ReadMe.en-US.md) | [Русский](ReadMe.ru-RU.md) | Bahasa Indonesia

![ScreenCapture-Plus](banner.png)

# ScreenCapture-Plus

**ScreenCapture-Plus v0.0.1** adalah cabang pemeliharaan [xland/ScreenCapture](https://github.com/xland/ScreenCapture), aplikasi tangkapan layar native Windows. Fitur mencakup gambar anotasi, tangkapan panjang, rekaman GIF / MP4, pembacaan kode QR, serta catatan bernomor yang diekspor bersama gambar.

## Bilah alat dan catatan

- Bilah alat tangkapan, gambar, rekaman, tangkapan panjang, dan gaya memakai **area klik 44 × 44 piksel logis dan ikon 24 piksel logis**. Ukuran mengikuti DPI monitor; tombol berpindah baris pada layar sempit tanpa mengecilkan area klik.
- Alat **Nomor** yang sudah ada tetap tersedia. Alat **Anotasi** baru di sebelahnya memakai ikon gelembung percakapan bernomor.
- Nomor dan warna ditetapkan otomatis. Memindahkan, mengurungkan, atau mengulangi anotasi mempertahankan nomor, warna, dan catatannya; menghapus catatan lain tidak mengubah nomor yang tersisa.
- Menyimpan atau menyalin menghasilkan satu gambar berisi tangkapan, penanda, dan catatan di sisi kanan. Teks dibungkus, ukuran huruf menyesuaikan ruang, dan hasil dapat bertambah tinggi atau memakai kolom kanan tambahan agar seluruh catatan tetap termuat.
- Zoom pratinjau tidak menurunkan resolusi gambar asli saat diekspor. Jika hasil lengkap melebihi batas sumber daya, aplikasi menampilkan kegagalan dan mempertahankan sesi penyuntingan.

### Menambahkan catatan

1. Pilih area tangkapan, lalu buka editor lewat tombol gambar / pin. Tahan `Ctrl` saat memilih area untuk langsung membuka editor.
2. Pilih **Anotasi** di sebelah **Nomor**.
3. Klik gambar untuk menambahkan penanda, lalu ketik catatan di panel kanan. Teks dapat terdiri dari beberapa baris.
4. Klik penanda atau entri daftar untuk menyunting catatannya; seret penanda untuk memindahkannya.
5. Gunakan tombol Simpan atau Salin untuk mengekspor seluruh gambar. `Ctrl+S` juga menyimpan saat menulis catatan. Di kolom teks, `Ctrl+C` menyalin teks; tekan `Esc` sebelum memakainya untuk menyalin seluruh gambar.

Daftar catatan dapat digulir. Menyembunyikan panel tidak menghapus catatan dan tidak menghilangkannya dari hasil ekspor. Lihat [panduan anotasi](ANNOTATIONS.md).

## Fitur lainnya

- Tangkapan biasa dan panjang, rekaman GIF / MP4; MP4 mendukung pilihan audio sistem dan mikrofon.
- Pembacaan kode QR dengan pilihan menyalin hasil.
- Pipet warna: `Ctrl+R` untuk RGB, `Ctrl+H` untuk HEX, `Ctrl+K` untuk CMYK.
- Persegi panjang, elips, panah, nomor, garis, teks, mosaik, dan penghapus; tahan `Shift` untuk membatasi bentuk atau arah garis.
- Arahkan kursor ke elemen untuk mengubah atau menghapusnya; `Ctrl+Z` mengurungkan dan `Ctrl+Y` mengulangi.
- `Ctrl+S` menyimpan, `Ctrl+C` atau klik dua kali menyalin; kolom teks menangani pintasannya sendiri saat sedang digunakan.
- `Ctrl` + roda mouse mengubah zoom pratinjau; tersedia tray, mode sekali pakai, dan beberapa bahasa.
- Jalankan satu `ScreenCapture.exe` tanpa instalasi. Dependensi aplikasi ditautkan statis sehingga tidak perlu memasang runtime Visual C++ secara terpisah.

## Unduh dan persyaratan

[Unduh v0.0.1](https://github.com/NAMEWTA/ScreenCapture-Plus/releases/tag/v0.0.1) · [Semua rilis](https://github.com/NAMEWTA/ScreenCapture-Plus/releases)

Aset rilis meliputi EXE, ZIP portabel Windows x64, ZIP simbol debug, `build-info.json`, dan `SHA256SUMS.txt`. ZIP portabel menyertakan bahasa dan lisensi pihak ketiga. Ukuran file mengikuti aset rilis yang tersedia.

Memerlukan **Windows 10 1803 atau lebih baru, x64**. Tutup proses versi lama sebelum memperbarui aplikasi secara manual.

## Build dan verifikasi

Gunakan Windows x64, Visual Studio 2026 dengan Desktop development with C++ dan ATL, PowerShell 7, Git, serta Rustup. Skrip mengambil revisi Ling / Yoga / gifski yang sudah dikunci.

```powershell
pwsh ./scripts/build.ps1 -Configuration Release
pwsh ./scripts/test.ps1
pwsh ./scripts/test-windows.ps1
pwsh ./scripts/smoke.ps1
pwsh ./scripts/test-windows.ps1 -Gui
pwsh ./scripts/package.ps1
```

EXE berada di `artifacts/bin/x64/Release/ScreenCapture.exe`. Buka `ScreenCapture.slnx` setelah build pertama untuk debugging. Uji algoritma, rendering native / PNG, dan startup tidak menggantikan pemeriksaan interaksi desktop, DPI campuran, IME, serta audio/video pada [daftar verifikasi Windows](VALIDATION.md).

Lihat [build dan rilis](BUILDING.md), [arsitektur](ARCHITECTURE.md), [kontribusi](../CONTRIBUTING.md), serta [lisensi pihak ketiga](../third_party/NOTICE.md).

## Baris perintah

```powershell
# Keluar setelah tugas tangkapan selesai
ScreenCapture.exe --auto-quit=true
# Buka editor setelah memilih area
ScreenCapture.exe --enter=pin
# Tangkapan panjang
ScreenCapture.exe --enter=long
# Rekaman layar
ScreenCapture.exe --enter=video
# Kode QR; --enter=qrcode juga diterima
ScreenCapture.exe --enter=qr
# Tray saja
ScreenCapture.exe --enter=tray
# Gabungkan opsi
ScreenCapture.exe --enter=long --auto-quit=true
```

## Pengaturan portabel dan bahasa

Pengaturan bawaan berada di `%appdata%\ScreenCapture\config.json`. File `config.json` yang sudah ada di sebelah EXE memiliki prioritas. Bahasa tambahan dimuat dari folder `Lang` di sebelah EXE jika folder tersebut ada; jika tidak, aplikasi memakai `%appdata%\ScreenCapture\Lang`. Bahasa Mandarin Sederhana dan Inggris tersedia di dalam EXE. Lihat [pengaturan bahasa](../Lang/ReadMe.md).

## Penulis upstream dan dukungan

Atribusi penulis upstream, [lisensi MIT](../LICENSE), dan informasi dukungan tetap dipertahankan. Kontak dan kode QR berikut adalah milik penulis upstream.

<table>
  <tr>
    <td align="center">
      <img alt="Sponsor Alipay" src="./alipay.jpg" width="160" height="160">
      <p>Sponsor Alipay</p>
    </td>
    <td align="center">
      <img alt="Sponsor WeChat" src="./wechat.png" width="160" height="160">
      <p>Sponsor WeChat</p>
    </td>
    <td align="center">
      <img alt="WeChat Penulis" src="./author.jpg" width="160" height="160">
      <p>WeChat Penulis</p>
    </td>
    <td align="center">
      <img alt="QR Code Akun Resmi" src="./gongzhonghao.jpg" width="160" height="160">
      <p>Blog WeChat: Desktop Software</p>
    </td>
  </tr>
</table>

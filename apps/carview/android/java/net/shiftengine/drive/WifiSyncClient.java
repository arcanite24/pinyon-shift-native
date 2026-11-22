// Android LAN Wi-Fi Sync client for Shift Drive (Slice 13B).
// Discovers the desktop shift-launcher Wi-Fi Sync server via UDP broadcast on
// port 47616 ("SHIFT-ENGINE-DISCOVER 1") or accepts a manual host:port, then
// fetches /shift/v1/manifest and resumable HTTP Range (/shift/v1/file/<index>)
// downloads into Android/data/net.shiftengine.drive/files/ (play/, mods/, state/).
package net.shiftengine.drive;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public final class WifiSyncClient {
    public static final int DISCOVERY_PORT = 47616;
    public static final String DISCOVER_PROBE = "SHIFT-ENGINE-DISCOVER 1";

    public static final class DiscoveredHost {
        public final String ip;
        public final int port;
        public final String name;

        public DiscoveredHost(String ip, int port, String name) {
            this.ip = ip;
            this.port = port;
            this.name = name;
        }
    }

    public static DiscoveredHost discoverOnLan(int timeoutMs) {
        DatagramSocket sock = null;
        try {
            sock = new DatagramSocket();
            sock.setBroadcast(true);
            sock.setSoTimeout(timeoutMs);
            byte[] payload = DISCOVER_PROBE.getBytes(StandardCharsets.US_ASCII);
            DatagramPacket pkt = new DatagramPacket(
                    payload, payload.length, InetAddress.getByName("255.255.255.255"), DISCOVERY_PORT);
            sock.send(pkt);

            byte[] buf = new byte[256];
            DatagramPacket reply = new DatagramPacket(buf, buf.length);
            sock.receive(reply);
            String msg = new String(reply.getData(), 0, reply.getLength(), StandardCharsets.US_ASCII).trim();
            // Expected: "SHIFT-ENGINE-HERE 1 <port> <hostname>"
            String[] parts = msg.split(" ", 4);
            if (parts.length >= 3 && "SHIFT-ENGINE-HERE".equals(parts[0])) {
                int port = Integer.parseInt(parts[2]);
                String name = parts.length >= 4 ? parts[3] : reply.getAddress().getHostAddress();
                return new DiscoveredHost(reply.getAddress().getHostAddress(), port, name);
            }
        } catch (Exception ignored) {
        } finally {
            if (sock != null) sock.close();
        }
        return null;
    }

    public static String fetchManifest(String hostIp, int port, String pairingCode) throws IOException {
        URL url = new URL("http://" + hostIp + ":" + port + "/shift/v1/manifest");
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setRequestMethod("GET");
        conn.setConnectTimeout(5000);
        conn.setReadTimeout(15000);
        conn.setRequestProperty("X-Shift-Code", pairingCode);
        int code = conn.getResponseCode();
        if (code != 200) {
            throw new IOException("Manifest request failed with HTTP " + code);
        }
        try (InputStream in = new BufferedInputStream(conn.getInputStream());
             ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } finally {
            conn.disconnect();
        }
    }

    // Resumable file download using HTTP Range: bytes=<existing>- into filesRoot/<relativePath>.
    public static void downloadFileResumable(String hostIp, int port, String pairingCode,
                                             int fileIndex, File filesRoot, String relativePath,
                                             long expectedSize) throws IOException {
        File dest = new File(filesRoot, relativePath).getCanonicalFile();
        File rootCanon = filesRoot.getCanonicalFile();
        if (!dest.getPath().startsWith(rootCanon.getPath() + File.separator)) {
            throw new IOException("Rejected unsafe destination path: " + relativePath);
        }
        File parent = dest.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();

        long existing = dest.exists() ? dest.length() : 0L;
        if (expectedSize > 0 && existing == expectedSize) return;
        if (expectedSize > 0 && existing > expectedSize) existing = 0L;

        URL url = new URL("http://" + hostIp + ":" + port + "/shift/v1/file/" + fileIndex);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setRequestMethod("GET");
        conn.setConnectTimeout(5000);
        conn.setReadTimeout(30000);
        conn.setRequestProperty("X-Shift-Code", pairingCode);
        if (existing > 0) {
            conn.setRequestProperty("Range", "bytes=" + existing + "-");
        }
        int status = conn.getResponseCode();
        if (status != 200 && status != 206) {
            conn.disconnect();
            throw new IOException("File " + fileIndex + " failed with HTTP " + status);
        }
        long writeOffset = (status == 206) ? existing : 0L;
        try (InputStream in = new BufferedInputStream(conn.getInputStream());
             RandomAccessFile raf = new RandomAccessFile(dest, "rw")) {
            raf.seek(writeOffset);
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) raf.write(buf, 0, n);
        } finally {
            conn.disconnect();
        }
    }
}

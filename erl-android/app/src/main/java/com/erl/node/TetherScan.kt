package com.erl.node

import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.net.URL
import java.nio.ByteBuffer
import java.util.concurrent.ExecutorCompletionService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Finds the node while the phone's *Ethernet tethering* is on.
 *
 * With tethering the USB link stops being a network the phone uses and becomes a tethering
 * downstream: the phone runs its own subnet on it (e.g. 192.168.x.0/24), the node takes an
 * address there by DHCP (usb-uplink on the board) and 192.168.7.1 is no longer reachable. The
 * phone's side of the link keeps an address in that subnet, so the node is one of its hosts:
 * probe them for the node's status CGI. Apps reach tethered hosts directly (local routes), no
 * network binding needed. Blocking: run off the main thread.
 */
object TetherScan {
    // Names of USB/Ethernet tethering interfaces on the phone (Samsung: eth0 for Ethernet tethering)
    private val IFACE_PREFIXES = listOf("eth", "usb", "rndis", "ncm")
    private const val THREADS = 64
    private const val CONNECT_MS = 400
    private const val HTTP_MS = 1500

    /** What the last [find] saw, for the waiting screen: interfaces, subnets probed, result */
    @Volatile var report: String = ""
        private set

    /** The node's IPv4 address in a tethering subnet, or null. [last] is tried first. */
    fun find(last: String?): String? {
        val hosts = candidates()
        if (hosts.isEmpty()) {
            report = "interfaces: ${interfaces()}\nscan: no USB/Ethernet subnet to probe"
            return null
        }
        if (last != null && last in hosts && isNode(last)) {
            report = "interfaces: ${interfaces()}\nscan: node at $last"
            return last
        }
        val found = probe(hosts)
        report = "interfaces: ${interfaces()}\nscan: ${hosts.size} hosts, " + (found?.let { "node at $it" } ?: "no node answered")
        return found
    }

    /** Every up interface with its IPv4 addresses, e.g. "eth0 192.168.42.1/24, rmnet1 10.1.2.3/30" */
    fun interfaces(): String = try {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
            .filter { it.isUp && !it.isLoopback }
            .mapNotNull { nif ->
                val v4 = nif.interfaceAddresses.filter { it.address is Inet4Address }
                    .joinToString(" ") { "${it.address.hostAddress}/${it.networkPrefixLength}" }
                if (v4.isEmpty()) null else "${nif.name} $v4"
            }
            .joinToString(", ").ifEmpty { "none with IPv4" }
    } catch (e: Exception) {
        "unreadable (${e.javaClass.simpleName})"
    }

    private fun probe(hosts: List<String>): String? {
        val pool = Executors.newFixedThreadPool(THREADS)
        try {
            val done = ExecutorCompletionService<String?>(pool)
            hosts.forEach { h -> done.submit { if (isNode(h)) h else null } }
            repeat(hosts.size) {
                val found = done.poll(10, TimeUnit.SECONDS)?.get() ?: return@repeat
                return found
            }
            return null
        } finally {
            pool.shutdownNow()
        }
    }

    /** Every other host of the tethering subnets on the phone's USB/Ethernet interfaces */
    private fun candidates(): List<String> {
        val out = LinkedHashSet<String>()
        for (nif in NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()) {
            if (!nif.isUp || nif.isLoopback || IFACE_PREFIXES.none { nif.name.startsWith(it) }) continue
            for (ia in nif.interfaceAddresses) {
                val addr = ia.address as? Inet4Address ?: continue
                val prefix = ia.networkPrefixLength.toInt()
                if (prefix !in 24..30) continue            // at most 254 hosts to probe
                val own = ByteBuffer.wrap(addr.address).int
                val net = own and (-1 shl (32 - prefix))
                for (i in 1 until (1 shl (32 - prefix)) - 1) {
                    val h = net or i
                    if (h != own) out += InetAddress.getByAddress(ByteBuffer.allocate(4).putInt(h).array()).hostAddress!!
                }
            }
        }
        return out.toList()
    }

    // A quick TCP connect filters out empty addresses, then the status CGI proves it is a node
    private fun isNode(host: String): Boolean = try {
        Socket().use { it.connect(InetSocketAddress(host, 80), CONNECT_MS) }
        val conn = URL("http://$host/cgi-bin/status").openConnection() as HttpURLConnection
        conn.connectTimeout = HTTP_MS
        conn.readTimeout = HTTP_MS
        try {
            val body = conn.inputStream.bufferedReader().use { it.readText() }
            conn.responseCode == 200 && "\"hostname\"" in body && "\"mesh\"" in body
        } finally {
            conn.disconnect()
        }
    } catch (e: Exception) {
        false
    }
}

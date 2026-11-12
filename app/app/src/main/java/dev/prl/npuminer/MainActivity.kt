package dev.prl.npuminer

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.Window
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            PrlTheme {
                val graph = remember { (application as PrlApp).graph }
                App(graph, window)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun App(graph: AppGraph, window: Window?) {
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val context = LocalContext.current

    val selfCheck = remember { mutableStateOf(SelfCheckState()) }
    val mine by graph.miner.state.collectAsState()
    val mon by graph.monitor.state.collectAsState()
    var screen by remember { mutableStateOf("check") }

    val notifPerm = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* foreground service still runs; notification may be silent */ }

        // If the miner is already running (activity recreated while service is
        // alive), skip the DSP test.
    LaunchedEffect(Unit) {
        if (graph.miner.isRunning) {
            selfCheck.value = SelfCheckState(done = true, passed = true, items = emptyList())
            graph.monitor.start()
            screen = "main"
            return@LaunchedEffect
        }
        // Let the activity reach the top-app cpuset first: the miner's prep
        // workers prefer CPUs 6/7, which are outside the launch-time
        // background cpuset on some OEM builds.
        selfCheck.value = SelfCheckState(running = true)
        delay(800)
        withContext(Dispatchers.Default) {
            graph.check.runAll { st -> selfCheck.value = st }
        }
        if (selfCheck.value.passed) {
            graph.monitor.start()
            screen = "main"
        }
    }

    DisposableEffect(mine.phase) {
        if (mine.phase == MinePhase.RUNNING || mine.phase == MinePhase.STARTING) {
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        onDispose { }
    }

    // If the miner exits on its own (RPC error, signal), drop the foreground
    // service so a stale "mining" notification is not left behind.
    LaunchedEffect(mine.phase) {
        if (mine.phase == MinePhase.STOPPED || mine.phase == MinePhase.ERROR) {
            context.stopService(Intent(context, MinerService::class.java))
        }
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(Pal.Bg, Color(0xFF0B1220))))
    ) {
        Scaffold(
            containerColor = Color.Transparent,
            snackbarHost = { SnackbarHost(snackbar) },
            topBar = {
                TopAppBar(
                    title = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text("PRL", style = MaterialTheme.typography.titleLarge)
                                Text("NPU MINER", style = MaterialTheme.typography.labelSmall, color = Pal.OnBgDim, letterSpacing = 2.sp)
                            }
                            Spacer(Modifier.width(10.dp))
                            StatusChip(mine, checking = screen == "check" && selfCheck.value.running)
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = Color.Transparent,
                        titleContentColor = MaterialTheme.colorScheme.onBackground,
                    ),
                )
            },
        ) { pad ->
            Box(
                Modifier.fillMaxSize().padding(pad),
                contentAlignment = Alignment.TopCenter,
            ) {
                if (screen == "check") {
                    CheckScreen(
                        state = selfCheck.value,
                        onRetry = {
                            scope.launch {
                                selfCheck.value = SelfCheckState(running = true)
                                withContext(Dispatchers.Default) {
                                    graph.check.runAll { st -> selfCheck.value = st }
                                }
                                if (selfCheck.value.passed) {
                                    graph.monitor.start()
                                    screen = "main"
                                }
                            }
                        },
                        modifier = Modifier.widthIn(max = 720.dp).fillMaxHeight(),
                    )
                } else {
                    Dashboard(
                        graph = graph,
                        mine = mine,
                        mon = mon,
                        onEvent = { msg -> scope.launch { snackbar.showSnackbar(msg) } },
                        onStartRequested = {
                            if (Build.VERSION.SDK_INT >= 33 &&
                                ContextCompat.checkSelfPermission(
                                    context, Manifest.permission.POST_NOTIFICATIONS
                                ) != PackageManager.PERMISSION_GRANTED
                            ) {
                                notifPerm.launch(Manifest.permission.POST_NOTIFICATIONS)
                            }
                            ContextCompat.startForegroundService(
                                context, Intent(context, MinerService::class.java)
                            )
                        },
                        onStopRequested = {
                            context.stopService(Intent(context, MinerService::class.java))
                        },
                        modifier = Modifier.widthIn(max = 720.dp).fillMaxHeight(),
                    )
                }
            }
        }
    }
}

@Composable
private fun SectionHeader(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    trailing: @Composable (() -> Unit)? = null,
) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(40.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primaryContainer),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(21.dp),
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        trailing?.invoke()
    }
}

@Composable
private fun SectionCard(content: @Composable () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(Modifier.padding(18.dp)) { content() }
    }
}

@Composable
private fun StatusChip(mine: MinerUiState, checking: Boolean = false) {
    val (text, color) = if (checking) {
        "检查中…" to Pal.Amber
    } else {
        when (mine.phase) {
            MinePhase.RUNNING -> if (mine.connected) "挖矿中" to Pal.Green else "连接中…" to Pal.Amber
            MinePhase.STARTING -> "启动中…" to Pal.Amber
            MinePhase.IDLE -> "未开始" to Pal.OnBgDim
            MinePhase.STOPPED -> "已停止" to Pal.OnBgDim
            MinePhase.ERROR -> "出错了" to Pal.Red
        }
    }
    Surface(
        shape = MaterialTheme.shapes.extraLarge,
        color = color.copy(alpha = 0.14f),
        contentColor = color,
    ) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.size(7.dp).background(color, CircleShape))
            Spacer(Modifier.width(6.dp))
            Text(text, style = MaterialTheme.typography.labelMedium)
        }
    }
}

/* ============================== self-check screen ========================= */

@Composable
fun CheckScreen(
    state: SelfCheckState,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Box(
            Modifier
                .size(56.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primaryContainer),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                Icons.Filled.Check,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(30.dp),
            )
        }
        Spacer(Modifier.height(14.dp))
        Text(
            "启动检查",
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            "正在确认这台手机是否支持 NPU 挖矿",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(20.dp))

        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.large,
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        ) {
            Column(Modifier.padding(vertical = 4.dp)) {
                state.items.forEachIndexed { index, item ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 18.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        CheckIcon(item.result)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(item.title, style = MaterialTheme.typography.titleSmall)
                            if (item.detail.isNotEmpty()) {
                                Text(
                                    item.detail,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                    if (index != state.items.lastIndex) {
                        HorizontalDivider(
                            Modifier.padding(horizontal = 18.dp),
                            color = MaterialTheme.colorScheme.outlineVariant,
                        )
                    }
                }
            }
        }

        Spacer(Modifier.height(16.dp))
        if (state.done && !state.passed) {
            val failed = state.items.filter { it.blocking && it.result == CheckResult.FAIL }
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = MaterialTheme.shapes.large,
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
            ) {
                Column(Modifier.padding(18.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Filled.Warning,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onErrorContainer,
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(
                            "这台手机暂不支持",
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.onErrorContainer,
                        )
                    }
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "以下检查未通过，暂时无法挖矿：",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer.copy(alpha = 0.8f),
                    )
                    failed.forEach { f ->
                        Text(
                            "• ${f.title}：${f.detail}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onErrorContainer,
                            modifier = Modifier.padding(top = 4.dp),
                        )
                    }
                }
            }
            Spacer(Modifier.height(14.dp))
        }

        if (state.done) {
            Button(
                onClick = onRetry,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                shape = MaterialTheme.shapes.medium,
            ) {
                Icon(Icons.Filled.Refresh, contentDescription = null, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text("重新检查", style = MaterialTheme.typography.titleSmall)
            }
        } else {
            LinearProgressIndicator(
                Modifier
                    .fillMaxWidth()
                    .clip(MaterialTheme.shapes.small),
                color = MaterialTheme.colorScheme.primary,
                trackColor = MaterialTheme.colorScheme.outlineVariant,
            )
        }
    }
}

@Composable
fun CheckIcon(r: CheckResult) {
    val bg = when (r) {
        CheckResult.PASS -> Pal.Green.copy(alpha = 0.15f)
        CheckResult.WARN -> Pal.Amber.copy(alpha = 0.15f)
        CheckResult.FAIL -> Pal.Red.copy(alpha = 0.15f)
        CheckResult.RUNNING -> MaterialTheme.colorScheme.primary.copy(alpha = 0.15f)
        CheckResult.SKIP -> MaterialTheme.colorScheme.surfaceContainerHigh
        CheckResult.PENDING -> MaterialTheme.colorScheme.surfaceContainerHigh
    }
    Box(
        Modifier.size(32.dp).clip(CircleShape).background(bg),
        contentAlignment = Alignment.Center,
    ) {
        when (r) {
            CheckResult.PASS -> Icon(
                Icons.Filled.Check, contentDescription = null,
                tint = Pal.Green, modifier = Modifier.size(18.dp),
            )
            CheckResult.WARN -> Icon(
                Icons.Filled.Warning, contentDescription = null,
                tint = Pal.Amber, modifier = Modifier.size(18.dp),
            )
            CheckResult.FAIL -> Icon(
                Icons.Filled.Close, contentDescription = null,
                tint = Pal.Red, modifier = Modifier.size(18.dp),
            )
            CheckResult.RUNNING -> CircularProgressIndicator(
                Modifier.size(16.dp),
                color = MaterialTheme.colorScheme.primary,
                strokeWidth = 2.dp,
            )
            CheckResult.SKIP -> Text(
                "–",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            CheckResult.PENDING -> Unit
        }
    }
}

/* ================================ dashboard =============================== */

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Dashboard(
    graph: AppGraph,
    mine: MinerUiState,
    mon: MonitorState,
    onEvent: (String) -> Unit,
    onStartRequested: () -> Unit,
    onStopRequested: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val cfgInit = remember { graph.store.load() }
    var pool by rememberSaveable { mutableStateOf(cfgInit.pool) }
    var wallet by rememberSaveable { mutableStateOf(cfgInit.wallet) }
    var worker by rememberSaveable { mutableStateOf(cfgInit.worker) }
    var dialect by rememberSaveable { mutableStateOf(cfgInit.dialect) }
    var pplns by rememberSaveable { mutableStateOf(cfgInit.pplns) }
    var savedFlash by remember { mutableStateOf(false) }
    var showRisk by remember { mutableStateOf(false) }
    var showAdvanced by rememberSaveable { mutableStateOf(false) }
    var settings by rememberSaveable { mutableStateOf(false) }
    val overviewScroll = rememberLazyListState()
    val settingsScroll = rememberLazyListState()

    val cfg = MineConfig(pool.trim(), wallet.trim(), worker.trim(), dialect, pplns)
    val cfgValid = validPool(cfg.pool) && validWallet(cfg.wallet) && validWorker(cfg.worker)
    val scope = rememberCoroutineScope()
    val context = LocalContext.current

    // "Remember the pool path": auto-save on every edit (debounced).
    LaunchedEffect(pool, wallet, worker, dialect, pplns) {
        delay(400)
        graph.store.save(
            MineConfig(pool.trim(), wallet.trim(), worker.trim(), dialect, pplns)
        )
        savedFlash = true
        delay(1200)
        savedFlash = false
    }

    val startMining: () -> Unit = {
        if (!cfgValid) {
            onEvent("请先填写正确的矿池地址和钱包地址")
        } else {
            graph.store.save(cfg)
            try {
                onStartRequested()
            } catch (_: Exception) {
                onEvent("后台服务启动失败，请重试")
            }
            val ok = graph.miner.start(cfg)
            if (!ok) {
                context.stopService(Intent(context, MinerService::class.java))
                onEvent("启动失败，请检查网络后重试")
            }
        }
    }

    Column(modifier.fillMaxSize().imePadding()) {
        SingleChoiceSegmentedButtonRow(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp)
        ) {
            SegmentedButton(
                selected = !settings, onClick = { settings = false },
                shape = SegmentedButtonDefaults.itemShape(0, 2),
            ) { Text("运行概览") }
            SegmentedButton(
                selected = settings, onClick = { settings = true },
                shape = SegmentedButtonDefaults.itemShape(1, 2),
            ) { Text("矿池设置") }
        }
        LazyColumn(
            Modifier.weight(1f).fillMaxWidth(),
            state = if (settings) settingsScroll else overviewScroll,
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (!settings) {
                item { HashrateCard(mine) }
                item { MonitorCard(mon) }
                item { SharesCard(mine) }
            }

            if (settings) item {
                SectionCard {
                    SectionHeader(
                        icon = Icons.Filled.Settings,
                        title = "矿池设置",
                        subtitle = "填写后自动保存",
                        trailing = {
                            Text(
                                if (savedFlash) "已保存" else "自动保存",
                                style = MaterialTheme.typography.labelSmall,
                                color = Pal.Green.copy(alpha = 0.8f),
                            )
                        },
                    )
                    Spacer(Modifier.height(14.dp))

                    OutlinedTextField(
                        value = pool, onValueChange = { pool = it },
                        label = { Text("矿池地址") },
                        placeholder = { Text(Store.POOL_HINT) },
                        singleLine = true,
                        shape = MaterialTheme.shapes.medium,
                        isError = pool.isNotBlank() && !validPool(pool.trim()),
                        supportingText = {
                            if (pool.isNotBlank() && !validPool(pool.trim())) {
                                Text("请填写矿池提供的地址，例如 example.miner.net:3333")
                            }
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(4.dp))
                    OutlinedTextField(
                        value = wallet, onValueChange = { wallet = it },
                        label = { Text("收款钱包地址") },
                        placeholder = { Text(Store.WALLET_HINT) },
                        singleLine = true,
                        shape = MaterialTheme.shapes.medium,
                        isError = wallet.isNotBlank() && !validWallet(wallet.trim()),
                        supportingText = {
                            if (wallet.isNotBlank() && !validWallet(wallet.trim())) {
                                Text("钱包地址通常以 prl1 开头")
                            }
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(4.dp))
                    OutlinedTextField(
                        value = worker, onValueChange = { worker = it },
                        label = { Text("矿工名称") },
                        singleLine = true,
                        shape = MaterialTheme.shapes.medium,
                        isError = worker.isNotBlank() && !validWorker(worker.trim()),
                        supportingText = {
                            if (worker.isNotBlank() && !validWorker(worker.trim())) {
                                Text("只能使用字母、数字、点、横线或下划线")
                            }
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )

                    Spacer(Modifier.height(14.dp))
                    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("协议格式", style = MaterialTheme.typography.labelLarge)
                        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                            SegmentedButton(
                                selected = dialect == "object",
                                onClick = { dialect = "object" },
                                shape = SegmentedButtonDefaults.itemShape(0, 2),
                            ) { Text("HeroMiners") }
                            SegmentedButton(
                                selected = dialect == "kryptex",
                                onClick = { dialect = "kryptex" },
                                shape = SegmentedButtonDefaults.itemShape(1, 2),
                            ) { Text("Kryptex") }
                        }
                    }
                    Text(
                        "按矿池要求选择；选错会连接失败",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 4.dp),
                    )

                    Spacer(Modifier.height(10.dp))
                    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("结算方式", style = MaterialTheme.typography.labelLarge)
                        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                            SegmentedButton(
                                selected = !pplns, onClick = { pplns = false },
                                shape = SegmentedButtonDefaults.itemShape(0, 2),
                            ) { Text("PPS+") }
                            SegmentedButton(
                                selected = pplns, onClick = { pplns = true },
                                shape = SegmentedButtonDefaults.itemShape(1, 2),
                            ) { Text("PPLNS") }
                        }
                    }
                    Text(
                        "以矿池网页端设置为准；这里只记录你的选择",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                    Text(
                        "当前矿工名：${cfg.effectiveWorker}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 6.dp),
                    )
                    if (mine.lastStopReason.isNotBlank() && mine.phase != MinePhase.RUNNING) {
                        Text(
                            "上次运行：${mine.lastStopReason}",
                            style = MaterialTheme.typography.bodySmall,
                            color = Pal.Amber,
                            modifier = Modifier.padding(top = 6.dp),
                        )
                    }
                    Text(
                        "开始挖矿前会提示高温风险",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 6.dp),
                    )

                    Spacer(Modifier.height(14.dp))
                    if (mine.phase == MinePhase.RUNNING) {
                        Text(
                            "修改会在下次开始挖矿时生效",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 6.dp),
                        )
                    }
                }
            }

            item {
                SectionCard {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 48.dp)
                            .clip(MaterialTheme.shapes.small)
                            .clickable { showAdvanced = !showAdvanced },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(
                            if (showAdvanced) Icons.Filled.KeyboardArrowUp else Icons.Filled.KeyboardArrowDown,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(
                            if (showAdvanced) "收起技术信息" else "显示技术信息",
                            style = MaterialTheme.typography.labelLarge,
                        )
                    }
                    if (showAdvanced) {
                        Text(
                            "v24 · 流体云通知 · 普通权限运行",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 10.dp),
                        )
                        Text(
                            "计算次数：${mine.attempts}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 4.dp),
                        )
                        Text(
                            "CPU 使用率为估算值（Android 16 对普通应用有限制）",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 4.dp),
                        )
                        if (mine.lastStopReason.isNotBlank()) {
                            Text(
                                "状态：${mine.lastStopReason}",
                                style = MaterialTheme.typography.bodySmall,
                                color = Pal.Amber,
                                modifier = Modifier.padding(top = 4.dp),
                            )
                        }
                        Spacer(Modifier.height(10.dp))
                        Text("运行日志", style = MaterialTheme.typography.titleSmall)
                        if (mine.log.isEmpty()) {
                            Text(
                                "暂无日志",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(top = 4.dp),
                            )
                        }
                        mine.log.forEach { ln ->
                            Text(
                                ln,
                                fontSize = 10.sp,
                                fontFamily = FontFamily.Monospace,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(top = 2.dp),
                            )
                        }
                    }
                }
            }
        }

        Surface(color = Pal.Surface, tonalElevation = 0.dp) {
            Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp)) {
                if (mine.phase == MinePhase.RUNNING || mine.phase == MinePhase.STARTING) {
                    Button(
                        onClick = {
                            scope.launch(Dispatchers.Default) { graph.miner.stop() }
                            onStopRequested()
                            onEvent("已停止挖矿")
                        },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.error,
                            contentColor = MaterialTheme.colorScheme.onError,
                        ),
                        shape = MaterialTheme.shapes.medium,
                        modifier = Modifier.fillMaxWidth().height(52.dp),
                    ) {
                        Icon(Icons.Filled.Close, contentDescription = null, modifier = Modifier.size(20.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("停止挖矿", style = MaterialTheme.typography.titleSmall)
                    }
                } else {
                    Button(
                        onClick = {
                            if (!cfgValid) {
                                settings = true
                            } else {
                                showRisk = true
                            }
                        },
                        shape = MaterialTheme.shapes.medium,
                        modifier = Modifier.fillMaxWidth().height(52.dp),
                    ) {
                        Icon(Icons.Filled.PlayArrow, contentDescription = null, modifier = Modifier.size(22.dp))
                        Spacer(Modifier.width(8.dp))
                        Text(
                            if (cfgValid) "开始挖矿" else "配置矿池与钱包",
                            style = MaterialTheme.typography.titleSmall,
                        )
                    }
                }
            }
        }
    }

    // Explicit risk acknowledgement: no automatic thermal protection anymore.
    if (showRisk) {
        var accepted by remember { mutableStateOf(false) }
        var remaining by remember { mutableStateOf(5) }
        LaunchedEffect(Unit) {
            while (remaining > 0) {
                delay(1000)
                remaining -= 1
            }
        }
        AlertDialog(
            onDismissRequest = { showRisk = false },
            title = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Filled.Warning, contentDescription = null, tint = Pal.Amber)
                    Spacer(Modifier.width(8.dp))
                    Text("开始前请确认")
                }
            },
            text = {
                Column {
                    Text(
                        "挖矿会让手机 AI 芯片长时间高负载运行，手机可能明显发热、降频或卡顿。" +
                                "应用不会自动停止挖矿，请确保手机通风散热，并随时留意" +
                                "“设备状态”里的温度。",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Spacer(Modifier.height(12.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(
                            checked = accepted,
                            onCheckedChange = { accepted = it },
                        )
                        Text("我已阅读并确认以上风险", style = MaterialTheme.typography.bodyMedium)
                    }
                    Text(
                        if (remaining > 0) "请先阅读以上说明，${remaining} 秒后可继续"
                        else "现在可以继续",
                        style = MaterialTheme.typography.labelMedium,
                        color = if (remaining > 0) Pal.Amber else Pal.Green,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        showRisk = false
                        startMining()
                    },
                    enabled = accepted && remaining <= 0,
                ) {
                    Text(if (remaining > 0) "继续挖矿（${remaining}s）" else "继续挖矿")
                }
            },
            dismissButton = {
                TextButton(onClick = { showRisk = false }) { Text("取消") }
            },
        )
    }
}

@Composable
fun HashrateCard(mine: MinerUiState) {
    val shape = MaterialTheme.shapes.large
    Box(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(
                Brush.verticalGradient(
                    listOf(
                        MaterialTheme.colorScheme.primary.copy(alpha = 0.18f),
                        MaterialTheme.colorScheme.surfaceContainer,
                    )
                )
            )
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
            .padding(18.dp)
    ) {
        Column {
            SectionHeader(
                icon = Icons.Filled.Star,
                title = "实时算力",
                subtitle = if (mine.phase == MinePhase.RUNNING) "每秒采样 · TMAC/s" else "等待下一次运行",
            )
            Spacer(Modifier.height(10.dp))
            val hasRate = mine.phase == MinePhase.RUNNING || mine.phase == MinePhase.STOPPED ||
                    mine.phase == MinePhase.ERROR
            Text(
                if (hasRate) "%.3f".format(mine.gmacs / 1000.0) else "0.000",
                style = MaterialTheme.typography.displaySmall.copy(fontFeatureSettings = "tnum", fontSize = 48.sp, lineHeight = 56.sp, letterSpacing = (-1.5).sp),
                color = if (hasRate) Pal.Primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                if (mine.phase == MinePhase.STOPPED || mine.phase == MinePhase.ERROR) "TMAC/s · 上次读数" else "TMAC/s",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            HashrateChart(mine.history)
        }
    }
}

@Composable
fun HashrateChart(history: List<Double>) {
    val h = history
    val rawMax = (h.maxOrNull() ?: 1.0).coerceAtLeast(0.001) * 1.15
    val ymax = animateFloatAsState(
        targetValue = rawMax.toFloat(),
        animationSpec = tween(500), label = "ymax",
    ).value.toDouble()
    val desc = if (h.isEmpty()) {
        "算力图表，暂无数据"
    } else {
        "算力图表，最近 %d 秒，最高 %.3f TMAC/s".format(
            h.size, (h.maxOrNull() ?: 0.0) / 1000.0
        )
    }
    val grid = MaterialTheme.colorScheme.outlineVariant

    Canvas(
        Modifier
            .fillMaxWidth()
            .height(150.dp)
            .clip(MaterialTheme.shapes.medium)
            .background(MaterialTheme.colorScheme.surfaceContainerLowest)
            .semantics { contentDescription = desc }
    ) {
        val w = size.width
        val ht = size.height
        val padL = 10f; val padT = 8f; val padB = 12f; val padR = 10f
        val plotW = w - padL - padR
        val plotH = ht - padT - padB
        for (i in 0..3) {
            val y = padT + plotH * i / 3f
            drawLine(grid, Offset(padL, y), Offset(padL + plotW, y), 1f)
        }
        if (h.isNotEmpty()) {
            val n = h.size
            val px = { i: Int ->
                if (n == 1) padL + plotW / 2f else padL + plotW * i / (n - 1)
            }
            val py = { v: Double ->
                (padT + plotH * (1.0 - (v / ymax)).coerceIn(0.0, 1.0)).toFloat()
            }
            if (h.size >= 2) {
                val path = Path()
                for (i in h.indices) {
                    if (i == 0) path.moveTo(px(i), py(h[i])) else path.lineTo(px(i), py(h[i]))
                }
                val fill = Path().apply {
                    addPath(path)
                    lineTo(px(n - 1), padT + plotH)
                    lineTo(px(0), padT + plotH)
                    close()
                }
                drawPath(
                    fill,
                    Brush.verticalGradient(
                        listOf(Pal.Primary.copy(alpha = 0.30f), Color.Transparent),
                        startY = padT, endY = padT + plotH,
                    ),
                )
                drawPath(path, Pal.Primary, style = Stroke(2.5f, cap = StrokeCap.Round))
            }
            drawCircle(Pal.Primary, 4f, Offset(px(n - 1), py(h.last())))
        }
    }
    Text(
        if (h.isEmpty()) "启动挖矿后，这里会显示算力曲线"
        else "最近 %d 秒 · 最高 %.3f TMAC/s".format(
            h.size, (h.maxOrNull() ?: 0.0) / 1000.0
        ),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 6.dp),
    )
}

@Composable
fun MonitorCard(mon: MonitorState) {
    SectionCard {
        SectionHeader(
            icon = Icons.Filled.Info,
            title = "设备状态",
            subtitle = "温度与资源占用",
        )
        Spacer(Modifier.height(14.dp))
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Tachometer("CPU 使用率（估算）", mon.cpuUsagePct, Modifier.weight(1f))
            Tachometer("内存使用率", mon.memUsedPct, Modifier.weight(1f))
        }
        Spacer(Modifier.height(12.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Metric(Modifier.weight(1f), "AI 芯片温度", mon.npuTempC, "°C", warnAt = 85f, hotAt = 95f)
            Metric(Modifier.weight(1f), "处理器温度", mon.cpuTempC, "°C", warnAt = 75f, hotAt = 90f)
        }
        Spacer(Modifier.height(10.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Metric(Modifier.weight(1f), "电池温度", mon.batteryTempC, "°C", warnAt = 40f, hotAt = 48f)
            Metric(Modifier.weight(1f), "功耗", mon.powerW, "W", fmt = { "%.2f W".format(it) })
        }
    }
}

@Composable
fun Tachometer(label: String, pct: Float, modifier: Modifier = Modifier) {
    val known = pct.isFinite()
    val target = if (known) pct.coerceIn(0f, 100f) else 0f
    val animated by animateFloatAsState(target, tween(650), label = label)
    val accent = when {
        target >= 90f -> Pal.Red
        target >= 70f -> Pal.Amber
        else -> Pal.Primary
    }
    Column(
        modifier.clip(MaterialTheme.shapes.medium)
            .background(Pal.SurfaceHi).padding(14.dp)
            .semantics { contentDescription = if (known) "$label ${pct.roundToInt()}%" else "$label 暂无数据" }
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = Pal.OnBgDim)
        Text(
            if (known) "%.0f%%".format(pct) else "—",
            style = MaterialTheme.typography.headlineSmall.copy(fontFeatureSettings = "tnum"),
            color = Pal.OnBg,
            modifier = Modifier.padding(vertical = 8.dp),
        )
        LinearProgressIndicator(
            progress = { animated / 100f },
            modifier = Modifier.fillMaxWidth().height(4.dp).clip(CircleShape),
            color = accent, trackColor = Pal.Outline,
        )
    }
}

@Composable
fun Metric(
    modifier: Modifier,
    label: String,
    value: Float,
    unit: String,
    warnAt: Float = Float.MAX_VALUE,
    hotAt: Float = Float.MAX_VALUE,
    fmt: ((Float) -> String)? = null,
) {
    val txt = if (!value.isFinite()) "—" else fmt?.invoke(value) ?: "%.1f%s".format(value, unit)
    val color = when {
        value.isNaN() || fmt != null -> MaterialTheme.colorScheme.onSurface
        value >= hotAt -> MaterialTheme.colorScheme.error
        value >= warnAt -> Pal.Amber
        else -> MaterialTheme.colorScheme.onSurface
    }
    Column(
        modifier
            .clip(MaterialTheme.shapes.medium)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .padding(12.dp)
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(2.dp))
        Text(
            txt,
            style = MaterialTheme.typography.titleLarge.copy(fontFeatureSettings = "tnum"),
            color = color,
        )
    }
}

@Composable
fun SharesCard(mine: MinerUiState) {
    val total = mine.accepted + mine.rejected
    val rejRate = if (total > 0) mine.rejected.toFloat() / total else 0f
    SectionCard {
        SectionHeader(
            icon = Icons.Filled.Check,
            title = "份额",
            subtitle = "本次会话",
        )
        Spacer(Modifier.height(14.dp))
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ShareStat("有效份额", mine.accepted, Pal.Green, Modifier.weight(1f))
            VerticalDivider(Modifier.height(44.dp), color = MaterialTheme.colorScheme.outlineVariant)
            ShareStat("无效份额", mine.rejected, Pal.Red, Modifier.weight(1f))
            VerticalDivider(Modifier.height(44.dp), color = MaterialTheme.colorScheme.outlineVariant)
            Column(
                Modifier.weight(1f),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    if (total > 0) "%.1f%%".format(rejRate * 100f) else "—",
                    style = MaterialTheme.typography.headlineSmall.copy(fontFeatureSettings = "tnum"),
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    "无效占比",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        Box(
            Modifier
                .fillMaxWidth()
                .height(6.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.outlineVariant)
        ) {
            if (rejRate > 0f) {
                Box(
                    Modifier
                        .fillMaxWidth(rejRate)
                        .fillMaxHeight()
                        .background(MaterialTheme.colorScheme.error)
                )
            }
        }
    }
}

@Composable
private fun ShareStat(label: String, value: Long, color: Color, modifier: Modifier = Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            "$value",
            style = MaterialTheme.typography.headlineSmall.copy(fontFeatureSettings = "tnum"),
            color = color,
        )
        Text(
            label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

fun validPool(s: String): Boolean =
    Regex("^(tls://)?[A-Za-z0-9._\\-]+:\\d{1,5}$").matches(s)

fun validWallet(s: String): Boolean =
    Regex("^[a-z0-9]{10,80}$").matches(s)

fun validWorker(s: String): Boolean =
    Regex("^[A-Za-z0-9._\\-]{1,30}$").matches(s)

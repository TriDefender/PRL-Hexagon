package com.tridefender.npupearl

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// Blue-black palette (fixed dark, per design request).
object Pal {
    val Bg = Color(0xFF070B12)
    val Surface = Color(0xFF0E141D)
    val SurfaceHi = Color(0xFF151E2B)
    val Outline = Color(0xFF243247)
    val Primary = Color(0xFF5B9DFF)
    val PrimaryDim = Color(0xFF1E4C9A)
    val OnBg = Color(0xFFE1EAF7)
    val OnBgDim = Color(0xFF8CA0BA)
    val Green = Color(0xFF4ADE80)
    val Red = Color(0xFFFF6B7A)
    val Amber = Color(0xFFFFC15E)
}

private val BlueBlackScheme = darkColorScheme(
    primary = Pal.Primary,
    onPrimary = Color(0xFF001B3D),
    primaryContainer = Color(0xFF123A73),
    onPrimaryContainer = Color(0xFFD9E6FF),
    inversePrimary = Color(0xFF2D5FA8),
    secondary = Color(0xFF9DB9E8),
    onSecondary = Color(0xFF0B1B33),
    secondaryContainer = Color(0xFF1B2C47),
    onSecondaryContainer = Color(0xFFD9E6FF),
    tertiary = Color(0xFF7FD1C8),
    onTertiary = Color(0xFF00201D),
    tertiaryContainer = Color(0xFF0F3B37),
    onTertiaryContainer = Color(0xFFB8F0E9),
    background = Pal.Bg,
    onBackground = Pal.OnBg,
    surface = Pal.Bg,
    onSurface = Pal.OnBg,
    surfaceVariant = Pal.SurfaceHi,
    onSurfaceVariant = Pal.OnBgDim,
    surfaceTint = Pal.Primary,
    inverseSurface = Pal.OnBg,
    inverseOnSurface = Pal.Bg,
    error = Pal.Red,
    onError = Color(0xFF3A0910),
    errorContainer = Color(0xFF3A0D15),
    onErrorContainer = Color(0xFFFFD9DD),
    outline = Pal.Outline,
    outlineVariant = Color(0xFF1B2636),
    scrim = Color(0xFF000000),
    surfaceBright = Color(0xFF1F2B3C),
    surfaceDim = Color(0xFF070B12),
    surfaceContainerLowest = Color(0xFF070B12),
    surfaceContainerLow = Color(0xFF0E141D),
    surfaceContainer = Color(0xFF131B26),
    surfaceContainerHigh = Color(0xFF192331),
    surfaceContainerHighest = Color(0xFF1F2B3C),
)

private val AppTypography = Typography(
    displaySmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 36.sp,
        lineHeight = 44.sp,
        letterSpacing = (-0.4).sp,
    ),
    headlineMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 28.sp,
        lineHeight = 34.sp,
    ),
    headlineSmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 24.sp,
        lineHeight = 30.sp,
    ),
    titleLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 22.sp,
        lineHeight = 28.sp,
    ),
    titleMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 16.sp,
        lineHeight = 22.sp,
    ),
    titleSmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 14.sp,
        lineHeight = 20.sp,
    ),
    bodyLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontSize = 16.sp,
        lineHeight = 24.sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontSize = 14.sp,
        lineHeight = 20.sp,
    ),
    bodySmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontSize = 12.sp,
        lineHeight = 16.sp,
    ),
    labelLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 14.sp,
        lineHeight = 20.sp,
    ),
    labelMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Medium,
        fontSize = 12.sp,
        lineHeight = 16.sp,
    ),
    labelSmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Medium,
        fontSize = 11.sp,
        lineHeight = 14.sp,
    ),
)

private val AppShapes = Shapes(
    extraSmall = RoundedCornerShape(6.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(22.dp),
    extraLarge = RoundedCornerShape(28.dp),
)

@Composable
fun PrlTheme(content: @Composable () -> Unit) {
    // Fixed blue-black brand theme (no dynamic color: the user asked for this palette first).
    MaterialTheme(
        colorScheme = BlueBlackScheme,
        typography = AppTypography,
        shapes = AppShapes,
        content = content,
    )
}

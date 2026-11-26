// Custom vector Shift Engine emblem view matching the official Shift Engine
// brand mark: 8-tooth precision gear silhouette with negative-space winding
// S-road and shifter apex pin dot in Horizon crimson-magenta (#E8195E).
package net.shiftengine.drive;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.View;

public class ShiftLogoView extends View {
    private static final int COL_TILE_TOP = Color.parseColor("#211A22");
    private static final int COL_TILE_BOT = Color.parseColor("#111318");
    private static final int COL_BORDER = Color.parseColor("#342A36");
    private static final int COL_ACCENT = Color.parseColor("#E8195E");

    private final Paint mTileFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mTileStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mMarkPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mTileRect = new RectF();
    private final Path mMarkPath = new Path();
    private float mCornerRadius = 8f;

    public ShiftLogoView(Context context) {
        super(context);
        mTileFill.setStyle(Paint.Style.FILL);
        mTileStroke.setStyle(Paint.Style.STROKE);
        mTileStroke.setColor(COL_BORDER);
        mMarkPaint.setStyle(Paint.Style.FILL);
        mMarkPaint.setColor(COL_ACCENT);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        float s = Math.min(w, h);
        if (s <= 0f) return;
        float ox = (w - s) * 0.5f;
        float oy = (h - s) * 0.5f;

        float strokeW = Math.max(1f, s * 0.025f);
        mTileStroke.setStrokeWidth(strokeW);
        mTileRect.set(ox + strokeW * 0.5f, oy + strokeW * 0.5f,
                ox + s - strokeW * 0.5f, oy + s - strokeW * 0.5f);
        mCornerRadius = s * 0.22f;

        mTileFill.setShader(new LinearGradient(
                ox, oy, ox + s * 0.4f, oy + s, COL_TILE_TOP, COL_TILE_BOT, Shader.TileMode.CLAMP));

        // Exact 512x512 vector geometry from assets/branding/shift-engine-mark.svg
        float pad = s * 0.08f;
        float k = (s - 2f * pad) / 512f;
        float bx = ox + pad;
        float by = oy + pad;

        mMarkPath.reset();
        mMarkPath.moveTo(bx + 187.12f * k, by + 89.70f * k);
        mMarkPath.lineTo(bx + 215.51f * k, by + 80.61f * k);
        mMarkPath.lineTo(bx + 214.79f * k, by + 43.97f * k);
        mMarkPath.lineTo(bx + 297.21f * k, by + 43.97f * k);
        mMarkPath.lineTo(bx + 296.49f * k, by + 80.61f * k);
        mMarkPath.lineTo(bx + 324.88f * k, by + 89.70f * k);
        mMarkPath.lineTo(bx + 351.39f * k, by + 103.35f * k);
        mMarkPath.lineTo(bx + 376.79f * k, by + 76.93f * k);
        mMarkPath.lineTo(bx + 435.07f * k, by + 135.21f * k);
        mMarkPath.lineTo(bx + 408.65f * k, by + 160.61f * k);
        mMarkPath.lineTo(bx + 422.30f * k, by + 187.12f * k);
        mMarkPath.lineTo(bx + 431.39f * k, by + 215.51f * k);
        mMarkPath.lineTo(bx + 468.03f * k, by + 214.79f * k);
        mMarkPath.lineTo(bx + 468.03f * k, by + 297.21f * k);
        mMarkPath.lineTo(bx + 431.39f * k, by + 296.49f * k);
        mMarkPath.lineTo(bx + 422.30f * k, by + 324.88f * k);
        mMarkPath.lineTo(bx + 408.65f * k, by + 351.39f * k);
        mMarkPath.lineTo(bx + 435.07f * k, by + 376.79f * k);
        mMarkPath.lineTo(bx + 376.79f * k, by + 435.07f * k);
        mMarkPath.lineTo(bx + 351.39f * k, by + 408.65f * k);
        mMarkPath.lineTo(bx + 324.88f * k, by + 422.30f * k);
        mMarkPath.lineTo(bx + 296.49f * k, by + 431.39f * k);
        mMarkPath.lineTo(bx + 297.21f * k, by + 468.03f * k);
        mMarkPath.lineTo(bx + 222.54f * k, by + 468.03f * k);
        mMarkPath.cubicTo(
                bx + 219.85f * k, by + 431.53f * k,
                bx + 223.47f * k, by + 400.73f * k,
                bx + 233.00f * k, by + 378.00f * k);
        mMarkPath.cubicTo(
                bx + 252.00f * k, by + 333.00f * k,
                bx + 306.00f * k, by + 312.00f * k,
                bx + 306.00f * k, by + 277.00f * k);
        mMarkPath.cubicTo(
                bx + 306.00f * k, by + 247.00f * k,
                bx + 266.00f * k, by + 238.00f * k,
                bx + 266.00f * k, by + 227.00f * k);
        mMarkPath.lineTo(bx + 266.00f * k, by + 190.41f * k);
        mMarkPath.cubicTo(
                bx + 278.78f * k, by + 186.21f * k,
                bx + 288.00f * k, by + 174.18f * k,
                bx + 288.00f * k, by + 160.00f * k);
        mMarkPath.cubicTo(
                bx + 288.00f * k, by + 142.33f * k,
                bx + 273.67f * k, by + 128.00f * k,
                bx + 256.00f * k, by + 128.00f * k);
        mMarkPath.cubicTo(
                bx + 238.33f * k, by + 128.00f * k,
                bx + 224.00f * k, by + 142.33f * k,
                bx + 224.00f * k, by + 160.00f * k);
        mMarkPath.cubicTo(
                bx + 224.00f * k, by + 174.18f * k,
                bx + 233.22f * k, by + 186.21f * k,
                bx + 246.00f * k, by + 190.41f * k);
        mMarkPath.lineTo(bx + 246.00f * k, by + 232.00f * k);
        mMarkPath.cubicTo(
                bx + 246.00f * k, by + 248.00f * k,
                bx + 275.00f * k, by + 252.00f * k,
                bx + 275.00f * k, by + 274.00f * k);
        mMarkPath.cubicTo(
                bx + 275.00f * k, by + 301.00f * k,
                bx + 220.00f * k, by + 315.00f * k,
                bx + 194.00f * k, by + 358.00f * k);
        mMarkPath.cubicTo(
                bx + 183.89f * k, by + 374.85f * k,
                bx + 177.51f * k, by + 394.00f * k,
                bx + 172.83f * k, by + 414.94f * k);
        mMarkPath.lineTo(bx + 160.61f * k, by + 408.65f * k);
        mMarkPath.lineTo(bx + 135.21f * k, by + 435.07f * k);
        mMarkPath.lineTo(bx + 76.93f * k, by + 376.79f * k);
        mMarkPath.lineTo(bx + 103.35f * k, by + 351.39f * k);
        mMarkPath.lineTo(bx + 89.70f * k, by + 324.88f * k);
        mMarkPath.lineTo(bx + 80.61f * k, by + 296.49f * k);
        mMarkPath.lineTo(bx + 43.97f * k, by + 297.21f * k);
        mMarkPath.lineTo(bx + 43.97f * k, by + 214.79f * k);
        mMarkPath.lineTo(bx + 80.61f * k, by + 215.51f * k);
        mMarkPath.lineTo(bx + 89.70f * k, by + 187.12f * k);
        mMarkPath.lineTo(bx + 103.35f * k, by + 160.61f * k);
        mMarkPath.lineTo(bx + 76.93f * k, by + 135.21f * k);
        mMarkPath.lineTo(bx + 135.21f * k, by + 76.93f * k);
        mMarkPath.lineTo(bx + 160.61f * k, by + 103.35f * k);
        mMarkPath.close();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (mTileRect.isEmpty()) return;
        canvas.drawRoundRect(mTileRect, mCornerRadius, mCornerRadius, mTileFill);
        canvas.drawRoundRect(mTileRect, mCornerRadius, mCornerRadius, mTileStroke);
        canvas.drawPath(mMarkPath, mMarkPaint);
    }
}

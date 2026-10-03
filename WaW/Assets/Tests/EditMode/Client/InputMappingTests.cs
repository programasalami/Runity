using System;
using NUnit.Framework;

namespace WaW.Client.Tests
{
    public class InputMappingTests
    {
        [Test]
        public void UnrotatedInputMapsToCompassDirections()
        {
            Assert.AreEqual(((sbyte)0, (sbyte)-127), InputMapping.ToWorldDirection(0, 1, 0));   // up = north = -y
            Assert.AreEqual(((sbyte)127, (sbyte)0), InputMapping.ToWorldDirection(1, 0, 0));    // right = east
            Assert.AreEqual(((sbyte)0, (sbyte)127), InputMapping.ToWorldDirection(0, -1, 0));   // down = south
        }

        [Test]
        public void DiagonalsAreNormalisedNotFaster()
        {
            var (x, y) = InputMapping.ToWorldDirection(1, 1, 0);
            Assert.AreEqual(90, x);
            Assert.AreEqual(-90, y);
        }

        [Test]
        public void ARotatedCameraRotatesTheInput()
        {
            // Camera turned a quarter: "up" on screen now walks east in the world.
            var (x, y) = InputMapping.ToWorldDirection(0, 1, MathF.PI / 2);
            Assert.AreEqual(127, x);
            Assert.AreEqual(0, y);
        }

        [Test]
        public void SmallStickNoiseIsIgnored()
        {
            Assert.AreEqual(((sbyte)0, (sbyte)0), InputMapping.ToWorldDirection(0.1f, 0.05f, 0));
        }
    }
}
